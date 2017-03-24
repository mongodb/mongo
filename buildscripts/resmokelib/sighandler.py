"""
Utility to support asynchronously signaling the current process.
"""

from __future__ import absolute_import

import atexit
import os
import signal
import sys
import threading
import traceback

_is_windows = (sys.platform == "win32")
if _is_windows:
    import win32api
    import win32event

from . import reportfile


def register(logger, suites):
    """
    On Windows, set up an event object to wait for signal, otherwise, register a signal handler
    for the SIGUSR1 signal.
    """

    def _handle_sigusr1(signum, frame):
        """
        Signal handler that will dump the stacks of all threads and
        then write out the report file.
        """

        header_msg = "Dumping stacks due to SIGUSR1 signal"

        _dump_stacks(logger, header_msg)
        reportfile.write(suites)

    def _handle_set_event(event_handle):
        """
        Windows event object handler that will dump the stacks of all threads and then write out
        the report file.
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

                _dump_stacks(logger, header_msg)
                reportfile.write(suites)


    # On Windows spawn a thread to wait on an event object for signal to dump stacks. For Cygwin
    # platforms, we use a signal handler since it supports POSIX signals.
    if _is_windows:
        # Create unique event_name.
        event_name = "Global\\Mongo_Python_" + str(os.getpid())

        try:
            security_attributes = None
            manual_reset = False
            initial_state = False
            task_timeout_handle = win32event.CreateEvent(security_attributes,
                                                         manual_reset,
                                                         initial_state,
                                                         event_name)
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
    """
    Signal handler that will dump the stacks of all threads.
    """

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
