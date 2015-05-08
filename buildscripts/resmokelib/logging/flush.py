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


def start_thread():
    """
    Starts the flush thread.
    """
    _FlushThread().start()


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
        # atexit handler is already set up to flush any loggers still in the queue when exiting.
        self.daemon = True

    def run(self):
        """
        Continuously shuts down loggers from the queue.
        """

        while True:
            logger = _LOGGER_QUEUE.get()
            _FlushThread._shutdown_logger(logger)

    @staticmethod
    def _shutdown_logger(logger):
        """
        Flushes and closes all handlers of 'logger'.
        """

        for handler in logger.handlers:
            handler.flush()
            handler.close()
