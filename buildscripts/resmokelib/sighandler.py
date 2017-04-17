"""
Utility to support asynchronously signaling the current process.
"""

from __future__ import absolute_import

import signal
import sys
import traceback

from . import reportfile


def register(logger, suites):
    """
    Registers a signal handler for the SIGUSR1 signal.
    """

    def _handle_sigusr1(signum, frame):
        """
        Signal handler that will dump the stacks of all threads and
        then write out the report file.
        """

        _dump_stacks(logger)
        reportfile.write(suites)

    try:
        signal.signal(signal.SIGUSR1, _handle_sigusr1)
    except AttributeError:
        logger.warn("Cannot catch signals on Windows")


def _dump_stacks(logger):
    """
    Signal handler that will dump the stacks of all threads.
    """

    header_msg = "Dumping stacks due to SIGUSR1 signal"

    sb = []
    sb.append(header_msg)

    frames = sys._current_frames()
    sb.append("Total threads: %d" % (len(frames)))
    sb.append("")

    for thread_id in frames:
        stack = frames[thread_id]
        sb.append("Thread %d:" % (thread_id))
        sb.append("".join(traceback.format_stack(stack)))

    logger.info("\n".join(sb))
