import json
import random
import signal
import sys
import time
import uuid

import click
import redis
from structlog import get_logger

logger = get_logger()


RESULT_KEY = "completion_queue"


class Worker:
    def __init__(self):
        pass

    def __call__(self, *args):
        time.sleep(1./35.)


@click.command()
@click.option("--redis-ip", default="0.0.0.0")
@click.option("--redis-port", required=True, default=7000, type=int)
@click.option("--is-fractional", is_flag=True)
@click.option("--fractional-sleep", type=float)
def consume(redis_ip, redis_port, is_fractional, fractional_sleep):
    if is_fractional:
        assert fractional_sleep is not None

    r = redis.Redis(redis_ip, port=redis_port, decode_responses=True)
    queue_name = uuid.uuid4().hex
    worker = Worker()

    r.execute_command("mantis.add_queue", queue_name)

    counter = 0
    log_every = 10
    next_query = None
    fractional_prob = 0.0
    last_print = time.time()

    def work_on_single_query(next_query):
        nonlocal counter
        counter += 1
        if counter % log_every == 0:
            logger.msg(
                f"Work on query {counter}",
                is_fractional=is_fractional,
                fractional_prob=fractional_prob,
                counter=counter,
                queue_name=queue_name,
            )
        query = json.loads(next_query)
        query["_3_dequeue_time"] = time.time()
        worker(query.pop("payload"))
        r.execute_command("mantis.complete", json.dumps(query))

    def signal_handler(*args):
        nonlocal next_query
        logger.msg("SIGINT received Draining the queue...")
        r.execute_command("mantis.drop_queue", queue_name)

        logger.msg("Currently processing query is ", next_query)
        if next_query:
            work_on_single_query(next_query)

        items_left = r.llen(queue_name)
        logger.msg("{} item left, processing...".format(items_left))
        for _ in range(items_left):
            next_query = r.lpop(queue_name)
            work_on_single_query(next_query)
        logger.msg("All done!")
        sys.exit(0)

    # Handle SIGTERM
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        while True:
            if is_fractional:
                # Print out updated fractional value
                if time.time() - last_print > 10:
                    logger.msg(f"current fractional prob is {fractional_prob}")
                    last_print = time.time()
                # Check every time because fractional prob may be updated
                new_fractional_prob = r.get("fractional_prob")
                fractional_prob = (
                    float(new_fractional_prob)
                    if new_fractional_prob is not None
                    else 0.0
                )

                if random.random() <= fractional_prob:
                    time.sleep(fractional_sleep)
                    continue

            next_query = r.blpop(queue_name, timeout=1)
            if next_query is None:
                logger.msg("Timeout, retrying...")
                continue

            # if not None, next_query is format (key, value)
            _, next_query = next_query
            work_on_single_query(next_query)
            next_query = None
    # Handle SIGINT
    except KeyboardInterrupt:
        signal_handler()
