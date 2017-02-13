"""
Helper class to read output of a subprocess. Used to avoid deadlocks
from the pipe buffer filling up and blocking the subprocess while it's
being waited on.
"""

from __future__ import absolute_import

import threading


class LoggerPipe(threading.Thread):
    """
    Asynchronously reads the output of a subprocess and sends it to a
    logger.
    """

    # The start() and join() methods are not intended to be called directly on the LoggerPipe
    # instance. Since we override them for that effect, the super's version are preserved here.
    __start = threading.Thread.start
    __join = threading.Thread.join

    def __init__(self, logger, level, pipe_out):
        """
        Initializes the LoggerPipe with the specified logger, logging
        level to use, and pipe to read from.
        """

        threading.Thread.__init__(self)
        # Main thread should not call join() when exiting
        self.daemon = True

        self.__logger = logger
        self.__level = level
        self.__pipe_out = pipe_out

        self.__lock = threading.Lock()
        self.__condition = threading.Condition(self.__lock)

        self.__started = False
        self.__finished = False

        LoggerPipe.__start(self)

    def start(self):
        raise NotImplementedError("start should not be called directly")

    def run(self):
        """
        Reads the output from 'pipe_out' and logs each line to 'logger'.
        """

        with self.__lock:
            self.__started = True
            self.__condition.notify_all()

        # Close the pipe when finished reading all of the output.
        with self.__pipe_out:
            # Avoid buffering the output from the pipe.
            for line in iter(self.__pipe_out.readline, b""):
                # Convert the output of the process from a bytestring to a UTF-8 string, and replace
                # any characters that cannot be decoded with the official Unicode replacement
                # character, U+FFFD. The log messages of MongoDB processes are not always valid
                # UTF-8 sequences. See SERVER-7506.
                line = line.decode("utf-8", "replace")
                self.__logger.log(self.__level, line.rstrip())

        with self.__lock:
            self.__finished = True
            self.__condition.notify_all()

    def join(self, timeout=None):
        raise NotImplementedError("join should not be called directly")

    def wait_until_started(self):
        with self.__lock:
            while not self.__started:
                self.__condition.wait()

    def wait_until_finished(self):
        with self.__lock:
            while not self.__finished:
                self.__condition.wait()

        # No need to pass a timeout to join() because the thread should already be done after
        # notifying us it has finished reading output from the pipe.
        LoggerPipe.__join(self)  # Tidy up the started thread.
