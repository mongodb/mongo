"""Function to record resmoke start time into a yaml file."""

from buildscripts.resmokelib import utils

_START_TIME_FILE = ".resmoke_start_time.yml"


def setup_start_time(start_time_secs: float):
    """Persist resmoke start time to disk."""
    to_serialize = {"start_time": start_time_secs}
    utils.dump_yaml_file(to_serialize, _START_TIME_FILE)


def compare_start_time(cur_time_secs):
    """
    Return the difference between the current unix time in seconds and the start time in seconds.

    :param cur_time_secs: current unix time in seconds; can be obtained from time.time()
    :return: difference in seconds.
    """
    try:
        cur_timefile = utils.load_yaml_file(_START_TIME_FILE)
        start_time_secs = cur_timefile["start_time"]
    except (FileNotFoundError, KeyError) as erros:
        raise FileNotFoundError("resmoke.py did not successfully record its start time") from erros

    return cur_time_secs - start_time_secs
