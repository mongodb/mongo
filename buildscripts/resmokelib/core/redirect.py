"""Helper classes for chaining process output."""

import subprocess
import sys


class StdoutRewrite(object):
    """A helper class that will overwrite `sys.stdout` and write incoming data to an underlying stream.

    Like all things that manipulate the sys.stdout global, this class is not threadsafe with
    concurrent writers to `sys.stdout`.
    """

    def __init__(self, write_to):
        """Construct a StdoutRewrite that writes to `write_to`."""

        self.write_to = write_to
        self.saved_stdout = sys.stdout
        sys.stdout = self

    def __enter__(self):
        pass

    def __exit__(self, typ, value, traceback):
        sys.stdout.flush()
        sys.stdout = self.saved_stdout
        self.saved_stdout = None

    def write(self, item):
        """Write `item` to `write_to`."""

        # Don't assume the emit record lock is being held. Throw away new-line only messages. Append
        # a newline to messages lacking a newline.
        if item == "\n":
            return
        if item[-1] != "\n":
            item += "\n"
        self.write_to.write(item.encode("utf-8", "replace"))

    def flush(self):
        """Flush `write_to`."""

        self.write_to.flush()


class Pipe(object):
    """Spawns a process that reads from `sys.stdout` or a specified object that implements `read` and writes to a specified object that implements `write` and `flush`."""

    def __init__(self, cmd, read_from, write_to):
        """`read_from` can be `sys.stdout` or an object that implements a `read` method. `write_to` must implement `write` and `flush`."""

        if read_from == sys.__stdout__:
            # sys.stdout does not implement a `read` method so it cannot be passed as a `stdin`
            # variable. Use a `StdoutRewrite` object to write the spawned `stdin`.
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=write_to)
            self.rewrite = StdoutRewrite(self.proc.stdin)
        else:
            self.proc = subprocess.Popen(cmd, stdin=read_from, stdout=write_to)

    def get_stdin(self):
        """Return the stdin stream from the spawned process."""

        return self.proc.stdin

    def get_stdout(self):
        """Return the stdout stream from the spawned process."""

        return self.proc.stdout

    def wait(self):
        """Wait for the process to terminate. Returns the error code."""

        return self.proc.wait()
