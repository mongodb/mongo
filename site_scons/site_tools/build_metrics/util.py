import datetime
import time

_BUILD_METRIC_DATA = {}


def get_build_metric_dict():
    global _BUILD_METRIC_DATA
    return _BUILD_METRIC_DATA


def add_meta_data(env, key, value):
    get_build_metric_dict()[key] = value


def timestamp_now() -> int:
    return time.time_ns()
