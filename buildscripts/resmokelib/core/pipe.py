"""
Helper class to read output of a subprocess.

Used to avoid deadlocks from the pipe buffer filling up and blocking the subprocess while it's
being waited on.
"""
from textwrap import wrap
import threading
from typing import List

# Logkeeper only support log lines up to 4 MB, we want to be a little under that to account for
# extra metadata that gets sent along with the log message.
MAX_LOG_LINE = int(3.5 * 1024 * 1024)


class LoggerPipe(threading.Thread):
    """Asynchronously reads the output of a subprocess and sends it to a logger."""

    # The start() and join() methods are not intended to be called directly on the LoggerPipe
    # instance. Since we override them for that effect, the super's version are preserved here.
    __start = threading.Thread.start
    __join = threading.Thread.join

    def __init__(self, logger, level, pipe_out):
        """Initialize the LoggerPipe with the specified arguments."""

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
        """Start not implemented."""
        raise NotImplementedError("start should not be called directly")

    def run(self):
        """Read the output from 'pipe_out' and logs each line to 'logger'."""

        with self.__lock:
            self.__started = True
            self.__condition.notify_all()

        # Close the pipe when finished reading all of the output.
        with self.__pipe_out:
            # Avoid buffering the output from the pipe.
            for line in iter(self.__pipe_out.readline, b""):
                lines = self._format_line_for_logging(line)
                for entry in lines:
                    self.__logger.log(self.__level, entry)

        with self.__lock:
            self.__finished = True
            self.__condition.notify_all()

    def join(self, timeout=None):
        """Join not implemented."""
        raise NotImplementedError("join should not be called directly")

    def wait_until_started(self):
        """Wait until started."""
        with self.__lock:
            while not self.__started:
                self.__condition.wait()

    def wait_until_finished(self):
        """Wait until finished."""
        with self.__lock:
            while not self.__finished:
                self.__condition.wait()

        # No need to pass a timeout to join() because the thread should already be done after
        # notifying us it has finished reading output from the pipe.
        LoggerPipe.__join(self)  # Tidy up the started thread.

    @staticmethod
    def _format_line_for_logging(line_bytes: bytes) -> List[str]:
        """
        Convert the given byte array into string(s) to be send to the logger.

        If the size of the input is greater than the max size supported by logkeeper, we will
        split the input into multiple strings that are under the max supported size.

        :param line_bytes: Byte array of the line to send to the logger.
        :return: List of strings to send to logger.
        """
        # Replace null bytes in the output of the subprocess with a literal backslash ('\')
        # followed by a literal zero ('0') so tools like grep don't treat resmoke.py's
        # output as binary data.
        line_bytes = line_bytes.replace(b"\0", b"\\0")

        # Convert the output of the process from a bytestring to a UTF-8 string, and replace
        # any characters that cannot be decoded with the official Unicode replacement
        # character, U+FFFD. The log messages of MongoDB processes are not always valid
        # UTF-8 sequences. See SERVER-7506.
        line_str = line_bytes.decode("utf-8", "replace")
        line_str = line_str.rstrip()
        if len(line_str) > MAX_LOG_LINE:
            return wrap(
                line_str,
                MAX_LOG_LINE,
                expand_tabs=False,
                replace_whitespace=False,
                drop_whitespace=False,
                break_on_hyphens=False,
            )
        return [line_str]
