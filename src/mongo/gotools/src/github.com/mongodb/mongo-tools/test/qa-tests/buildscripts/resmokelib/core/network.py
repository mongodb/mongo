"""
Class used to allocate ports for use by various mongod and mongos
processes involved in running the tests.
"""

from __future__ import absolute_import

import collections
import functools
import threading

from .. import config
from .. import errors


def _check_port(func):
    """
    A decorator that verifies the port returned by the wrapped function
    is in the valid range.

    Returns the port if it is valid, and raises a PortAllocationError
    otherwise.
    """

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        port = func(*args, **kwargs)

        if port < 0:
            raise errors.PortAllocationError("Attempted to use a negative port")

        if port > PortAllocator.MAX_PORT:
            raise errors.PortAllocationError("Exhausted all available ports. Consider decreasing"
                                             " the number of jobs, or using a lower base port")

        return port

    return wrapper


class PortAllocator(object):
    """
    This class is responsible for allocating ranges of ports.

    It reserves a range of ports for each job with the first part of
    that range used for the fixture started by that job, and the second
    part of the range used for mongod and mongos processes started by
    tests run by that job.
    """

    # A PortAllocator will not return any port greater than this number.
    MAX_PORT = 2 ** 16 - 1

    # Each job gets a contiguous range of _PORTS_PER_JOB ports, with job 0 getting the first block
    # of ports, job 1 getting the second block, and so on.
    _PORTS_PER_JOB = 250

    # The first _PORTS_PER_FIXTURE ports of each range are reserved for the fixtures, the remainder
    # of the port range is used by tests.
    _PORTS_PER_FIXTURE = 10

    _NUM_USED_PORTS_LOCK = threading.Lock()

    # Used to keep track of how many ports a fixture has allocated.
    _NUM_USED_PORTS = collections.defaultdict(int)

    @classmethod
    @_check_port
    def next_fixture_port(cls, job_num):
        """
        Returns the next port for a fixture to use.

        Raises a PortAllocationError if the fixture has requested more
        ports than are reserved per job, or if the next port is not a
        valid port number.
        """
        with cls._NUM_USED_PORTS_LOCK:
            start_port = config.BASE_PORT + (job_num * cls._PORTS_PER_JOB)
            num_used_ports = cls._NUM_USED_PORTS[job_num]
            next_port = start_port + num_used_ports

            cls._NUM_USED_PORTS[job_num] += 1

            if next_port >= start_port + cls._PORTS_PER_FIXTURE:
                raise errors.PortAllocationError(
                    "Fixture has requested more than the %d ports reserved per fixture"
                    % cls._PORTS_PER_FIXTURE)

            return next_port

    @classmethod
    @_check_port
    def min_test_port(cls, job_num):
        """
        For the given job, returns the lowest port that is reserved for
        use by tests.

        Raises a PortAllocationError if that port is higher than the
        maximum port.
        """
        return config.BASE_PORT + (job_num * cls._PORTS_PER_JOB) + cls._PORTS_PER_FIXTURE

    @classmethod
    @_check_port
    def max_test_port(cls, job_num):
        """
        For the given job, returns the highest port that is reserved
        for use by tests.

        Raises a PortAllocationError if that port is higher than the
        maximum port.
        """
        next_range_start = config.BASE_PORT + ((job_num + 1) * cls._PORTS_PER_JOB)
        return next_range_start - 1
