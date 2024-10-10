import subprocess
import sys
import time

_BUILD_METRIC_DATA = {}


# This section is from the original
# https://stackoverflow.com/a/70693158/1644736
def fullname(o):
    try:
        # if o is a class or function, get module directly
        module = o.__module__
    except AttributeError:
        # then get module from o's class
        module = o.__class__.__module__
    try:
        # if o is a class or function, get name directly
        name = o.__qualname__
    except AttributeError:
        # then get o's class name
        name = o.__class__.__qualname__
    # if o is a method of builtin class, then module will be None
    if module == "builtins" or module is None:
        return name
    return module + "." + name


# This section is an excerpt of the original
# https://stackoverflow.com/a/63029332/1644736
class CaptureAtexits:
    def __init__(self):
        self.captured = []

    def __eq__(self, other):
        self.captured.append(other)
        return False


def mem_adjustment(mem_usage):
    # apparently macos big sur (11) changed some of the api for getting memory,
    # so the memory comes up a bit larger than expected. Testing shows it about
    # 10 times large then what native macos tools report, so we will do some
    # adjustment in the mean time until its fixed:
    # https://github.com/giampaolo/psutil/issues/1908
    try:
        if sys.platform == "darwin":
            mem_adjust_version = subprocess.run(
                ["sw_vers", "-productVersion"], capture_output=True, text=True, check=False
            ).stdout.split(".")[0]
            if int(mem_adjust_version) > 10:
                return int(mem_usage / 10)
    except (IndexError, ValueError):
        pass
    return mem_usage


def get_build_metric_dict():
    global _BUILD_METRIC_DATA
    return _BUILD_METRIC_DATA


def add_meta_data(env, key, value):
    get_build_metric_dict()[key] = value


def timestamp_now() -> int:
    return time.time_ns()
