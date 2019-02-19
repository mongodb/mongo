"""Utility to support asynchronously signaling the current process."""

import atexit
import os
import signal
import sys
import threading
import time
import traceback

_IS_WINDOWS = (sys.platform == "win32")
if _IS_WINDOWS:
    import win32api
    import win32event

from . import reportfile  # pylint: disable=wrong-import-position
from . import testing  # pylint: disable=wrong-import-position


def register(logger, suites, start_time):
    """Register an event object to wait for signal, or a signal handler for SIGUSR1."""

    def _handle_sigusr1(signum, frame):  # pylint: disable=unused-argument
        """Signal handler for SIGUSR1.

        The handler will dump the stacks of all threads and write out the report file and
        log suite summaries.
        """

        header_msg = "Dumping stacks due to SIGUSR1 signal"

        _dump_and_log(header_msg)

    def _handle_set_event(event_handle):
        """Event object handler for Windows.

        The handler will dump the stacks of all threads and write out the report file and
        log suite summaries.
        """

        while True:
            try:
                # Wait for task time out to dump stacks.
                ret = win32event.WaitForSingleObject(event_handle, win32event.INFINITE)
                if ret != win32event.WAIT_OBJECT_0:
                    logger.error("_handle_set_event WaitForSingleObject failed: %d" % ret)
                    return
            except win32event.error as err:
                logger.error("Exception from win32event.WaitForSingleObject with error: %s" % err)
            else:
                header_msg = "Dumping stacks due to signal from win32event.SetEvent"

                _dump_and_log(header_msg)

    def _dump_and_log(header_msg):
        """Dump the stacks of all threads, write report file, and log suite summaries."""
        _dump_stacks(logger, header_msg)
        reportfile.write(suites)

        testing.suite.Suite.log_summaries(logger, suites, time.time() - start_time)

    # On Windows spawn a thread to wait on an event object for signal to dump stacks. For Cygwin
    # platforms, we use a signal handler since it supports POSIX signals.
    if _IS_WINDOWS:
        # Create unique event_name.
        event_name = "Global\\Mongo_Python_" + str(os.getpid())

        try:
            security_attributes = None
            manual_reset = False
            initial_state = False
            task_timeout_handle = win32event.CreateEvent(security_attributes, manual_reset,
                                                         initial_state, event_name)
        except win32event.error as err:
            logger.error("Exception from win32event.CreateEvent with error: %s" % err)
            return

        # Register to close event object handle on exit.
        atexit.register(win32api.CloseHandle, task_timeout_handle)

        # Create thread.
        event_handler_thread = threading.Thread(target=_handle_set_event,
                                                kwargs={"event_handle": task_timeout_handle},
                                                name="windows_event_handler_thread")
        event_handler_thread.daemon = True
        event_handler_thread.start()
    else:
        # Otherwise register a signal handler
        signal.signal(signal.SIGUSR1, _handle_sigusr1)


def _dump_stacks(logger, header_msg):
    """Signal handler that will dump the stacks of all threads."""

    sb = []
    sb.append(header_msg)

    frames = sys._current_frames()  # pylint: disable=protected-access
    sb.append("Total threads: %d" % (len(frames)))
    sb.append("")

    for thread_id in frames:
        stack = frames[thread_id]
        sb.append("Thread %d:" % (thread_id))
        sb.append("".join(traceback.format_stack(stack)))

    logger.info("\n".join(sb))
