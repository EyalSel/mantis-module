import subprocess
import time
import json
import numpy as np
import redis
from structlog import get_logger
import msgpack
import sys
import os
from pathlib import Path

HOME = Path.home()
sys.path.append(str(HOME))


class Controller:
    def get_action_from_state(self,
                              e2e_latency_since_last_call,  # List[float] in ms , todo
                              interarrival_deltas_ms_since_last_call,  # np.diff(real_ts_ns) / 1e3
                              current_number_of_replicas,  # Only active replicas count
                              queue_length  # Sum(active replicas queue length)
                              ):
        pass


class DRLFeaturizer:
    """
    Violates some abstraction barrier in MetisEnv. The feature engineering function needs to be stripped out
    of MetisEnv if it's used in multiple places.
    """
    def __init__(self, time_step, service_rate, slo_sec):
        from Metis.rllib_run_cluster import MetisEnv
        from Metis.optimizer.profiler import LogicalDAG
        self.model_name = "temp-model-name"
        adj_list = {
            LogicalDAG.SOURCE: [self.model_name],
            self.model_name: [LogicalDAG.SINK],
            LogicalDAG.SINK: [],
        }
        env_config = {
            "service_rate": service_rate,
            "scale_factors": {self.model_name: 1.0},
            "dag": LogicalDAG(adj_list, self.model_name),
            "slo": slo_sec,
            "time_step": 1e12,  # Only used in an assert
        }
        self.env = MetisEnv(env_config)
        self.env.model_name = self.model_name
        self.env.inflate_queue = None

    def __call__(self, e2e_lats_ms, deltas_ms, num_replicas, qlen):
        state = {}
        state["e2e_lats"] = list(e2e_lats_ms)
        state["arrival_deltas"] = list(deltas_ms)
        state["current_config"] = {}
        state["current_config"][self.model_name] = {}
        state["current_config"][self.model_name]["rf"] = num_replicas
        state["current_config"][self.model_name]["future_rf_delta"] = 0
        state["queue_lengths"] = {}
        state["queue_lengths"][self.model_name] = qlen
        state["new_misses"] = None
        state["state_time"] = None
        return self.env._extract_features(json.dumps(state))


class DRLTestbedController(Controller):
    def __init__(self):
        self.name = "DRL"
        self.featurizer = DRLFeaturizer(time_step=5, service_rate=35, slo_sec=0.1)
        from Metis.metis_serve import DRLController
        checkpoint = HOME/"ray_results/PPO/PPO_MetisEnv_a454fbce_2019-12-27_22-45-55q365fagc/checkpoint_103/checkpoint-103"
        self.controller = DRLController(checkpoint)
        import time
        time.sleep(3)
        self.controller.new_episode()

    def get_action_from_state(self, e2e_lats_ms, deltas_ms, num_replicas, qlen):
        obs = self.featurizer(e2e_lats_ms, deltas_ms, num_replicas, qlen)
        print(obs)
        return self.controller.action_from_obs(obs)[0]


class BangBang(Controller):
    slo = 150  # ms
    low = 0.5 * slo
    high = 0.8 * slo

    def __init__(self):
        self.name = "BangBang"

    def get_action_from_state(self, e2e_lats_ms, deltas_ms, num_replicas, qlen):
        if len(e2e_lats_ms) == 0:
            return 0
        p99 = np.percentile(e2e_lats_ms, 99)
        if p99 < self.low:
            return -1
        if p99 > self.high:
            return 1
        return 0


def scale(new_reps):
    scale_cmd = ['kubectl', 'scale', '--replicas={}'.format(int(new_reps)), 'deploy/mantis-worker']
    result = subprocess.call(' '.join(scale_cmd), shell=True)
    assert result == 0


# ctl = BangBang()
ctl = DRLTestbedController()
RESULT_KEY = "completion_queue"
logger = get_logger()


def delete_file(filename):
    if os.path.exists(filename):
        os.remove(filename)


delete_file(ctl.name + "_lineage")
delete_file(ctl.name + "_metrics")

r = redis.Redis("0.0.0.0", port=7000, decode_responses=True)
while True:
    # Latency list
    length_to_pop = r.llen(RESULT_KEY)
    lineage = []
    summary = []
    arrivals = []
    for _ in range(length_to_pop):
        __, val = r.blpop(RESULT_KEY)
        parsed_msg = json.loads(val)
        summary.append(
            float(parsed_msg["_4_done_time"]) - (parsed_msg["_2_enqueue_time"])
        )
        arrivals.append(parsed_msg["_2_enqueue_time"])
        lineage.append(parsed_msg)
    if len(summary):
        percentiles = [25, 50, 95, 99, 100]
        logger.msg(
            "Received {} from last interval".format(len(summary)),
            **dict(zip(map(str, percentiles), np.percentile(summary, percentiles)))
        )
    else:
        logger.msg("No result received in 5sec")

    val = r.execute_command("mantis.status")

    decoded_msg = json.loads(val)
    msg = json.loads(val)
    non_scaler_metric = [
        "real_ts_ns",
        "queues",
        "queue_sizes",
        "dropped_queues",
    ]
    [decoded_msg.pop(metric) for metric in non_scaler_metric]
    logger.msg("Gathered metrics...", **decoded_msg)

    curr_reps = msg["num_active_replica"]
    action = ctl.get_action_from_state(
            e2e_lats_ms=np.array(summary)*1000.,
            deltas_ms=np.diff(msg["real_ts_ns"])/1e6,
            num_replicas=msg["num_active_replica"],
            qlen=sum(msg["queue_sizes"])
            )
    target_reps = curr_reps + action
    target_reps = max(1, target_reps)
    scale(target_reps)

    with open(ctl.name+"_lineage", 'ab') as f:
        f.write(msgpack.packb(lineage))

    with open(ctl.name+"_metrics", 'ab') as f:
        f.write(msgpack.packb(msg))

    time.sleep(5)
