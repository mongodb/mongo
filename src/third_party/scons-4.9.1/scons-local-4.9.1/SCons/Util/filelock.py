# MIT License
#
# Copyright The SCons Foundation

"""SCons file locking functions.

Simple-minded filesystem-based locking. Provides a context manager
which acquires a lock (or at least, permission) on entry and
releases it on exit.

Usage::

    from SCons.Util.filelock import FileLock

    with FileLock("myfile.txt", writer=True) as lock:
        print(f"Lock on {lock.file} acquired.")
        # work with the file as it is now locked
"""

# TODO: things to consider.
#   Is raising an exception the right thing for failing to get lock?
#   Is a filesystem lockfile scheme sufficient for our needs?
#   - or is it better to put locks on the actual file (fcntl/windows-based)?
#   ... Is that even viable in the case of a remote (network) file?
#   Is this safe enough? Or do we risk dangling lockfiles?
#   Permission issues in case of multi-user. This *should* be okay,
#     the cache usually goes in user's homedir, plus you already have
#     enough rights for the lockfile if the dir lets you create the cache.
#   Need a forced break-lock method?
#   The lock attributes could probably be made opaque. Showed one visible
#     in the example above, but not sure the benefit of that.

from __future__ import annotations

import os
import time


class SConsLockFailure(Exception):
    """Lock failure exception."""


class FileLock:
    """Lock a file using a lockfile.

    Basic locking for when multiple processes may hit an externally
    shared resource that cannot depend on locking within a single SCons
    process. SCons does not have a lot of those, but caches come to mind.

    Cross-platform safe, does not use any OS-specific features.  Provides
    context manager support, or can be called with :meth:`acquire_lock`
    and :meth:`release_lock`.

    Lock can be a write lock, which is held until released, or a read
    lock, which releases immediately upon aquisition - we want to not
    read a file which somebody else may be writing, but not create the
    writers starvation problem of the classic readers/writers lock.

    TODO: Should default timeout be None (non-blocking), or 0 (block forever),
       or some arbitrary number?

    Arguments:
       file: name of file to lock. Only used to build the lockfile name.
       timeout: optional time (sec) to give up trying.
          If ``None``, quit now if we failed to get the lock (non-blocking).
          If 0, block forever (well, a long time).
       delay: optional delay between tries [default 0.05s]
       writer: if True, obtain the lock for safe writing. If False (default),
          just wait till the lock is available, give it back right away.

    Raises:
        SConsLockFailure: if the operation "timed out", including the
          non-blocking mode.
    """

    def __init__(
        self,
        file: str,
        timeout: int | None = None,
        delay: float | None = 0.05,
        writer: bool = False,
    ) -> None:
        if timeout is not None and delay is None:
            raise ValueError("delay cannot be None if timeout is None.")
        # It isn't completely obvious where to put the lockfile.
        # This scheme depends on diffrent processes using the same path
        # to the lockfile, since the lockfile is the magic resource,
        # not the file itself. getcwd() is no good for testcases, each of
        # which run in a unique test directory. tempfile is no good,
        # as those are (intentionally) unique per process.
        # Our simple first guess is just put it where the file is.
        self.file = file
        self.lockfile = f"{file}.lock"
        self.lock: int | None = None
        self.timeout = 999999 if timeout == 0 else timeout
        self.delay = 0.0 if delay is None else delay
        self.writer = writer

    def acquire_lock(self) -> None:
        """Acquire the lock, if possible.

        If the lock is in use, check again every *delay* seconds.
        Continue until lock acquired or *timeout* expires.
        """
        start_time = time.perf_counter()
        while True:
            try:
                self.lock = os.open(self.lockfile, os.O_CREAT|os.O_EXCL|os.O_RDWR)
            except (FileExistsError, PermissionError) as exc:
                if self.timeout is None:
                    raise SConsLockFailure(
                        f"Could not acquire lock on {self.file!r}"
                    ) from exc
                if (time.perf_counter() - start_time) > self.timeout:
                    raise SConsLockFailure(
                        f"Timeout waiting for lock on {self.file!r}."
                    ) from exc
                time.sleep(self.delay)
            else:
                if not self.writer:
                    # reader: waits to get lock, but doesn't hold it
                    self.release_lock()
                break

    def release_lock(self) -> None:
        """Release the lock by deleting the lockfile."""
        if self.lock:
            os.close(self.lock)
            os.unlink(self.lockfile)
            self.lock = None

    def __enter__(self) -> FileLock:
        """Context manager entry: acquire lock if not holding."""
        if not self.lock:
            self.acquire_lock()
        return self

    def __exit__(self, exc_type, exc_value, exc_tb) -> None:
        """Context manager exit: release lock if holding."""
        if self.lock:
            self.release_lock()

    def __repr__(self) -> str:
        """Nicer display if someone repr's the lock class."""
        return (
            f"{self.__class__.__name__}("
            f"file={self.file!r}, "
            f"timeout={self.timeout!r}, "
            f"delay={self.delay!r}, "
            f"writer={self.writer!r})"
        )
