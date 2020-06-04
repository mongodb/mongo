"""Utility to support asynchronously signaling the current process."""

import atexit
import os
import signal
import sys
import threading
import time
import traceback

import psutil

from buildscripts.resmokelib import reportfile
from buildscripts.resmokelib import testing
from buildscripts.resmokelib import config
from buildscripts.resmokelib.hang_analyzer import hang_analyzer
from buildscripts.resmokelib import parser

_IS_WINDOWS = (sys.platform == "win32")
if _IS_WINDOWS:
    import win32api
    import win32event


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

        if 'is_inner_level' not in config.INTERNAL_PARAMS:
            # Gather and analyze pids of all subprocesses.
            # Do nothing for child resmoke process started by another resmoke process
            # (e.g. backup_restore.js) The child processes of the child resmoke will be
            # analyzed by the signal handler of the top-level resmoke process.
            # i.e. the next few lines of code.
            pids_to_analyze = _get_pids()
            _analyze_pids(logger, pids_to_analyze)

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


def _get_pids():
    """Return all PIDs spawned by the current resmoke process and their child PIDs."""
    pids = []  # Gather fixture PIDs + any PIDs spawned by the fixtures.
    parent = psutil.Process()  # current process
    for child in parent.children(recursive=True):
        # Don't signal python threads. They have already been signalled in the evergreen timeout
        # section.
        if 'python' not in child.name().lower():
            pids.append(child.pid)

    return pids


def _analyze_pids(logger, pids):
    """Analyze the PIDs spawned by the current resmoke process."""
    # If 'test_analysis' is specified, we will just write the pids out to a file and kill them
    # Instead of running analysis. This option will only be specified in resmoke selftests.
    if 'test_analysis' in config.INTERNAL_PARAMS:
        with open(os.path.join(config.DBPATH_PREFIX, "test_analysis.txt"), "w") as analysis_file:
            analysis_file.write("\n".join([str(pid) for pid in pids]))
            for pid in pids:
                try:
                    proc = psutil.Process(pid)
                    logger.info("Killing process pid %d", pid)
                    proc.kill()
                except psutil.NoSuchProcess:
                    # Process has already terminated.
                    pass

        return

    hang_analyzer_args = [
        'hang-analyzer', '-o', 'file', '-o', 'stdout', '-k', '-d', ','.join([str(p) for p in pids])
    ]

    if not os.getenv('ASAN_OPTIONS'):
        hang_analyzer_args.append('-c')
    _hang_analyzer = parser.parse_command_line(hang_analyzer_args, logger=logger)
    _hang_analyzer.execute()
