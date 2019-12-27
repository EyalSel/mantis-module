#include "glog/logging.h"
#include "json.hpp"
#include "redismodule.h"

#include <chrono>
#include <iostream>
#include <iterator>
#include <numeric>
#include <random>
#include <string_view>

#define CAT_I(a, b) a##b
#define STRCAT(a, b) CAT_I(a, b)
#define MantisCommand(name) STRCAT(STRCAT(Mantis_, name), _RedisCommand)

#define MANTIS_DEBUG

constexpr std::string_view ACTIVE_QUEUES = "active_queues";
constexpr std::string_view REMOVED_QUEUES = "removed_queues";
constexpr std::string_view REAL_TIMESTAMPS_NS = "real_timestamp";
constexpr std::string_view FRACTIONAL_PROB = "fractional_prob";
constexpr std::string_view COMPLETION_QUEUE = "completion_queue";

inline std::vector<std::string> get_array_reply(RedisModuleCallReply *data_array) {
  size_t data_length = RedisModule_CallReplyLength(data_array);

  std::vector<std::string> array;
  array.reserve(data_length);

  for (size_t i = 0; i < data_length; i++) {
    size_t str_len;
    const char *char_ptr = RedisModule_CallReplyStringPtr(
        RedisModule_CallReplyArrayElement(data_array, i), &str_len);
    std::string truncated(char_ptr, str_len);
    array.push_back(truncated);
  }

  return array;
}

inline long long get_current_time_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
inline long long get_queue_length(RedisModuleCtx *ctx, std::string queue_name) {
  RedisModuleCallReply *reply;
  reply = RedisModule_Call(ctx, "LLEN", "c", queue_name.c_str());
  long long length = RedisModule_CallReplyInteger(reply);
  return length;
}

bool queue_is_active(RedisModuleCtx *ctx, std::string queue_name) {
  RedisModuleCallReply *reply;
  reply =
      RedisModule_Call(ctx, "SISMEMBER", "cc", ACTIVE_QUEUES.data(), queue_name.c_str());
  long long is_active = RedisModule_CallReplyInteger(reply);
  return (is_active == 1);
}

inline std::vector<std::string> get_active_queues(RedisModuleCtx *ctx) {
  RedisModuleCallReply *reply;
  reply = RedisModule_Call(ctx, "SMEMBERS", "c", ACTIVE_QUEUES.data());
  std::vector<std::string> queues = get_array_reply(reply);
  return queues;
}

inline std::vector<std::string> get_removed_queus(RedisModuleCtx *ctx) {
  // Retrieve all members
  RedisModuleCallReply *reply;
  reply = RedisModule_Call(ctx, "SMEMBERS", "c", REMOVED_QUEUES.data());
  std::vector<std::string> queues = get_array_reply(reply);

  // Filter out those are queues are cleared already
  std::vector<std::string> still_not_empty_queues;
  std::vector<std::string> empty_queues;
  for (auto &name : queues) {
    reply = RedisModule_Call(ctx, "LLEN", "c", name.c_str());
    auto length = RedisModule_CallReplyInteger(reply);
    if (length == 0) {
      empty_queues.push_back(name);
    } else {
      still_not_empty_queues.push_back(name);
    }
  }

  for (auto &name : empty_queues) {
    reply = RedisModule_Call(ctx, "SREM", "cc", REMOVED_QUEUES.data(), name.c_str());
  }

  return still_not_empty_queues;
}

// mantis.enqueue payload lg_sent_time unique_id
int MantisCommand(ENQUEUE)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  LOG_FIRST_N(INFO,1) << "First ENQUEUE";

  // BEGIN: choose a queue
  std::vector<std::string> queues = get_active_queues(ctx);

  std::string chosen_queue_name;
  CHECK(queues.size() > 0) << "No queue available";
  if (queues.size() == 1) {
    chosen_queue_name = queues[0];
    // LOG(INFO) << "Only one queue avaiable, used " << chosen_queue_name;
  } else {
    std::vector<std::string> two_chosen_queues;
    std::sample(queues.begin(), queues.end(), std::back_inserter(two_chosen_queues), 2,
                std::mt19937{std::random_device{}()});

    long long first_queue_len = get_queue_length(ctx, two_chosen_queues[0]);
    long long second_queue_len = get_queue_length(ctx, two_chosen_queues[1]);
    chosen_queue_name =
        first_queue_len < second_queue_len ? two_chosen_queues[0] : two_chosen_queues[1];

    // #ifdef MANTIS_DEBUG
    //     nlohmann::json tmp;
    //     tmp["all_queues"] = queues;
    //     tmp["two_queues"] = two_chosen_queues;
    //     tmp["two_queues_length"] = std::vector<long long>{first_queue_len,
    //     second_queue_len}; tmp["chosen_queue"] = chosen_queue_name; LOG(INFO) <<
    //     tmp.dump();
    // #endif
  }
  // END: choose a queue

  // BEGIN: construct serialized_query
  RedisModuleString *payload_str = argv[1];
  RedisModuleString *sent_time_str = argv[2];
  RedisModuleString *unique_id = argv[3];
  long long unique_id_int;
  RedisModule_StringToLongLong(unique_id, &unique_id_int);

  std::string payload;
  size_t payload_len;
  double sent_time;

  payload = RedisModule_StringPtrLen(payload_str, &payload_len);
  RedisModule_StringToDouble(sent_time_str, &sent_time);

  auto current_time_ns = get_current_time_ns();
  auto current_time_s = static_cast<double>(current_time_ns) / 1.0e9;

  RedisModule_Call(ctx, "RPUSH", "cl", REAL_TIMESTAMPS_NS.data(), current_time_ns);

  nlohmann::json query;
  query["payload"] = payload;
  query["query_id"] = unique_id_int;
  query["_1_lg_sent"] = sent_time;
  query["_2_enqueue_time"] = current_time_s;
  query["_2_queue_name"] = chosen_queue_name;
  std::string serialized_query = query.dump();
  // END: construct serialized_query

  // BEGIN: enqueue serialized_query
  RedisModule_Call(ctx, "RPUSH", "cc", chosen_queue_name.c_str(),
                   serialized_query.c_str());
  // END: enqueue serialized_query

  RedisModule_ReplyWithNull(ctx);
  return REDISMODULE_OK;
}

// mantis.add_queue my-random-uuid-queue-name
int MantisCommand(ADD_QUEUE)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  LOG_FIRST_N(INFO,1) << "First ADD_QUEUE";

  // Note that queue name will be client generated.
  // We assume the client has already called "subscribe" to that queue.
  RedisModuleString *queue_name = argv[1];

  RedisModule_Call(ctx, "SADD", "cs", ACTIVE_QUEUES.data(), queue_name);

  RedisModule_ReplyWithNull(ctx);
  return REDISMODULE_OK;
}

// mantis.drop_queue my-random-uuid-queue-name
int MantisCommand(DROP_QUEUE)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  LOG_FIRST_N(INFO,1) << "First DROP_ENQUEUE";

  RedisModuleString *queue_name = argv[1];
  RedisModule_Call(ctx, "SADD", "cs", REMOVED_QUEUES.data(), queue_name);
  RedisModule_Call(ctx, "SREM", "cs", ACTIVE_QUEUES.data(), queue_name);

  RedisModule_ReplyWithNull(ctx);
  return REDISMODULE_OK;
}

// mantis.status
int MantisCommand(STATUS)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 1) return RedisModule_WrongArity(ctx);
  REDISMODULE_NOT_USED(argv);

  RedisModule_AutoMemory(ctx);
  LOG_FIRST_N(INFO,1) << "First STATUS";

  // Get timestamps in ns
  std::vector<long long> timestamps_ns;
  RedisModuleCallReply *lrange_reply;
  lrange_reply =
      RedisModule_Call(ctx, "LRANGE", "ccc", REAL_TIMESTAMPS_NS.data(), "0", "-1");
  std::vector<std::string> array = get_array_reply(lrange_reply);
  for (auto &item : array) {
    timestamps_ns.push_back(std::stoll(item));
  }
  size_t array_size = array.size();
  for (size_t i = 0; i < array_size; i++) {
    RedisModule_Call(ctx, "LPOP", "c", REAL_TIMESTAMPS_NS.data());
  }
  // End get timestamp

  // Begin get fractional_value
  RedisModuleCallReply *reply;
  reply = RedisModule_Call(ctx, "GET", "c", FRACTIONAL_PROB.data());
  double fractional_val;
  if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_NULL) {
    fractional_val = 0.0;
  } else {
    size_t len;
    std::string fractional_str = RedisModule_CallReplyStringPtr(reply, &len);
    fractional_val = std::stod(fractional_str);
  }
  // End get fractional_value

  // Get queue sizes
  // The total queue sizes are active + dropped queue sizes.
  std::vector<long long> queue_sizes;
  std::vector<std::string> queues = get_active_queues(ctx);
  for (auto &queue_name : queues) {
    queue_sizes.push_back(get_queue_length(ctx, queue_name));
  }

  std::vector<long long> dropped_queue_sizes;
  std::vector<std::string> dropped_queues = get_removed_queus(ctx);
  for (auto &queue_name : dropped_queues) {
    dropped_queue_sizes.push_back(get_queue_length(ctx, queue_name));
  }

  // Sum active queue size and dropped queue size
  long long total_queue_size = std::accumulate(queue_sizes.begin(), queue_sizes.end(), 0);
  total_queue_size +=
      std::accumulate(dropped_queue_sizes.begin(), dropped_queue_sizes.end(), 0);
  // End get queue sizes

  size_t active_replicas = queues.size();
  size_t dropped_replicas = dropped_queues.size();
  long long current_time = get_current_time_ns();

  nlohmann::json status_report;

  // Real deltas from last call. List[int]
  status_report["real_ts_ns"] = timestamps_ns;

  // List[UUID] 
  status_report["queues"] = queues;
  status_report["dropped_queues"] = dropped_queues;

  // Active queue sizes ActiveList[int]
  status_report["queue_sizes"] = queue_sizes;

  // Dropped queue sizes DropList[int] (Scaling down)
  status_report["dropped_queue_sizes"] = dropped_queue_sizes;

  // -------- Inferred ----------
  // Sum(ActiveList[int], DropList[int])
  status_report["total_queue_size"] = total_queue_size;

  // Int 
  status_report["num_active_replica"] = active_replicas;
  // Int
  status_report["num_dropped_replica"] = dropped_replicas;
  // Int = Int + Int
  status_report["num_total_replica"] = active_replicas+dropped_replicas;

  // -------------------------------------
  status_report["current_time_ns"] = current_time;

  // Float, configurable
  status_report["fractional_value"] = fractional_val;

  std::string report_string = status_report.dump();

  RedisModule_ReplyWithStringBuffer(ctx, report_string.c_str(), report_string.size());
  return REDISMODULE_OK;
}

// mantis.complete payload
// - query["_4_done_time"] = time.time()
// - r.lpush("completion_queue", json.dumps(query))
int MantisCommand(COMPLETE)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);

  RedisModule_AutoMemory(ctx);

  LOG_FIRST_N(INFO,1) << "First COMPLETE";

  RedisModuleString *payload_str = argv[1];
  size_t payload_len;
  std::string payload = RedisModule_StringPtrLen(payload_str, &payload_len);

  auto query = nlohmann::json::parse(payload);

  auto current_time_ns = get_current_time_ns();
  auto current_time_s = static_cast<double>(current_time_ns) / 1.0e9;
  query["_4_done_time"] = current_time_s;
  std::string serialized_query = query.dump();

  RedisModule_Call(ctx, "LPUSH", "cc", COMPLETION_QUEUE.data(), serialized_query.c_str());

  RedisModule_ReplyWithNull(ctx);
  return REDISMODULE_OK;
}

// Used to prevent module reload -> double init of glog.
bool is_glog_initialized = false;

extern "C" {
/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);

  FLAGS_logtostderr = 1;
  if (!is_glog_initialized) {
    google::InitGoogleLogging("mantis_redis_module");
    is_glog_initialized = true;
  }

  if (RedisModule_Init(ctx, "mantis", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx, "mantis.enqueue", MantisCommand(ENQUEUE), "write", 0,
                                0, 0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx, "mantis.add_queue", MantisCommand(ADD_QUEUE),
                                "write", 0, 0, 0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx, "mantis.drop_queue", MantisCommand(DROP_QUEUE),
                                "write", 0, 0, 0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx, "mantis.status", MantisCommand(STATUS), "readonly",
                                0, 0, 0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx, "mantis.complete", MantisCommand(COMPLETE), "write",
                                0, 0, 0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  return REDISMODULE_OK;
}
}
