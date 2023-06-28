#!/usr/bin/env python
#
# Modified by: Corey Goldberg, 2013
#   License: GPLv2+
#
# Original code from:
#   Bazaar (bzrlib.tests.__init__.py, v2.6, copied Jun 01 2013)
#   Copyright (C) 2005-2011 Canonical Ltd
#   License: GPLv2+

"""Python testtools extension for running unittest suites concurrently.

The `testtools` project provides a ConcurrentTestSuite class, but does
not provide a `make_tests` implementation needed to use it.

This allows you to parallelize a test run across a configurable number
of worker processes. While this can speed up CPU-bound test runs, it is
mainly useful for IO-bound tests that spend most of their time waiting for
data to arrive from someplace else and can benefit from cocncurrency.

Unix only.
"""

import os
import sys
import time
import traceback
from threading import Thread
import unittest
from itertools import cycle
from multiprocessing import cpu_count

from subunit import ProtocolTestCase, TestProtocolClient
from subunit.test_results import AutoTimingTestResultDecorator

from testtools import ConcurrentTestSuite, iterate_tests


_all__ = [
    'ConcurrentTestSuite',
    'fork_for_tests',
    'partition_tests',
]

# This file has been modified from the original concurrencytest-0.1.2 to wait for the child processes
# preventing defunct processes and to prefix output with the running PID for debuggability.

CPU_COUNT = cpu_count()

def wait_for_children(pids):
    while pids:
        try:
            # As Windows doesn't support -1 for all children, loop through each child pid explicitly.
            for child_pid in pids:
                pid, exit_status = os.waitpid(child_pid, os.WNOHANG)
                exit_code = exit_status
                if "waitstatus_to_exitcode" in dir(os):
                    # This is only supported from Python 3.9 which is what our automated tests use.
                    # Without it, the results are harder to understand.
                    # See https://docs.python.org/3/library/os.html#os.waitstatus_to_exitcode
                    exit_code = os.waitstatus_to_exitcode(exit_status)
                if exit_code != 0:
                    pids.remove(pid)
                    if exit_code > 0:
                        print("[pid:{}]: Unexpected exit ({}) for child process ({})".format(os.getpid(), exit_code, pid))
                    else:
                        print("[pid:{}]: Unexpected exit by signal ({}) for child process ({})".format(os.getpid(), abs(exit_code), pid))
        except ChildProcessError:
            # No children processes.
            break
        # Sleep as the waipid is non blocking.
        time.sleep(5)

def fork_for_tests(concurrency_num=CPU_COUNT):
    """Implementation of `make_tests` used to construct `ConcurrentTestSuite`.

    :param concurrency_num: number of processes to use.
    """
    def do_fork(suite):
        """Take suite and start up multiple runners by forking (Unix only).

        :param suite: TestSuite object.

        :return: An iterable of TestCase-like objects which can each have
        run(result) called on them to feed tests to result.
        """
        result = []
        test_blocks = partition_tests(suite, concurrency_num)
        # Clear the tests from the original suite so it doesn't keep them alive
        suite._tests[:] = []
        pids = []
        for process_tests in test_blocks:
            process_suite = unittest.TestSuite(process_tests)
            # Also clear each split list so new suite has only reference
            process_tests[:] = []
            c2pread, c2pwrite = os.pipe()
            pid = os.fork()
            if pid == 0:
                try:
                    stream = os.fdopen(c2pwrite, 'wb', 1)
                    os.close(c2pread)
                    # Leave stderr and stdout open so we can see test noise
                    # Close stdin so that the child goes away if it decides to
                    # read from stdin (otherwise its a roulette to see what
                    # child actually gets keystrokes for pdb etc).
                    sys.stdin.close()
                    subunit_result = AutoTimingTestResultDecorator(
                        TestProtocolClient(stream)
                    )
                    # Set the pid tag for the parent to log with this information.
                    subunit_result.tags(["pid:" + str(os.getpid())], [])
                    process_suite.run(subunit_result)
                except:
                    # Try and report traceback on stream, but exit with error
                    # even if stream couldn't be created or something else
                    # goes wrong.  The traceback is formatted to a string and
                    # written in one go to avoid interleaving lines from
                    # multiple failing children.
                    try:
                        print("[pid:{}]: {}".format(os.getpid(), traceback.format_exc()))
                    finally:
                        os._exit(1)
                os._exit(0)
            else:
                os.close(c2pwrite)
                stream = os.fdopen(c2pread, 'rb', 1)
                test = ProtocolTestCase(stream)
                result.append(test)
                pids.append(pid)
            # Monitor our children to prevent leaving <defunct> processes around.
            wait_thread = Thread(target = wait_for_children, args = (pids, ))
            wait_thread.start()
        return result
    return do_fork


def partition_tests(suite, count):
    """Partition suite into count lists of tests."""
    # This just assigns tests in a round-robin fashion.  On one hand this
    # splits up blocks of related tests that might run faster if they shared
    # resources, but on the other it avoids assigning blocks of slow tests to
    # just one partition.  So the slowest partition shouldn't be much slower
    # than the fastest.
    partitions = [list() for _ in range(count)]
    tests = iterate_tests(suite)
    for partition, test in zip(cycle(partitions), tests):
        partition.append(test)
    return partitions

if __name__ == '__main__':
    import time

    class SampleTestCase(unittest.TestCase):
        """Dummy tests that sleep for demo."""

        def test_me_1(self):
            time.sleep(0.5)

        def test_me_2(self):
            time.sleep(0.5)

        def test_me_3(self):
            time.sleep(0.5)

        def test_me_4(self):
            time.sleep(0.5)

    # Load tests from SampleTestCase defined above
    suite = unittest.TestLoader().loadTestsFromTestCase(SampleTestCase)
    runner = unittest.TextTestRunner()

    # Run tests sequentially
    runner.run(suite)

    # Run same tests across 4 processes
    concurrent_suite = ConcurrentTestSuite(suite, fork_for_tests(4))
    runner.run(concurrent_suite)
