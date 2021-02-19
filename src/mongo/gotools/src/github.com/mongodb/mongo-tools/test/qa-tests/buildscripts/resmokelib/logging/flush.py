"""
Workaround for having too many threads running on 32-bit systems when
logging to buildlogger that still allows periodically flushing messages
to the buildlogger server.

This is because a utils.timer.AlarmClock instance is used for each
buildlogger.BuildloggerTestHandler, but only dismiss()ed when the Python
process is about to exit.
"""

from __future__ import absolute_import

import threading

from ..utils import queue


_LOGGER_QUEUE = queue.Queue()

_FLUSH_THREAD_LOCK = threading.Lock()
_FLUSH_THREAD = None


def start_thread():
    """
    Starts the flush thread.
    """

    global _FLUSH_THREAD
    with _FLUSH_THREAD_LOCK:
        if _FLUSH_THREAD is not None:
            raise ValueError("FlushThread has already been started")

        _FLUSH_THREAD = _FlushThread()
        _FLUSH_THREAD.start()


def stop_thread():
    """
    Signals the flush thread to stop and waits until it does.
    """

    with _FLUSH_THREAD_LOCK:
        if _FLUSH_THREAD is None:
            raise ValueError("FlushThread hasn't been started")

    # Add sentinel value to indicate when there are no more loggers to process.
    _LOGGER_QUEUE.put(None)
    _FLUSH_THREAD.join()


def close_later(logger):
    """
    Adds 'logger' to the queue so that it is closed later by the flush
    thread.
    """
    _LOGGER_QUEUE.put(logger)


class _FlushThread(threading.Thread):
    """
    Asynchronously flushes and closes logging handlers.
    """

    def __init__(self):
        """
        Initializes the flush thread.
        """

        threading.Thread.__init__(self, name="FlushThread")
        # Do not wait to flush the logs if interrupted by the user.
        self.daemon = True

    def run(self):
        """
        Continuously shuts down loggers from the queue.
        """

        while True:
            logger = _LOGGER_QUEUE.get()
            try:
                if logger is None:
                    # Sentinel value received, so exit.
                    break
                _FlushThread._shutdown_logger(logger)
            finally:
                _LOGGER_QUEUE.task_done()

    @staticmethod
    def _shutdown_logger(logger):
        """
        Flushes and closes all handlers of 'logger'.
        """

        for handler in logger.handlers:
            handler.flush()
            handler.close()
