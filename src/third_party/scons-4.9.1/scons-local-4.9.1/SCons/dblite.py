# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""
dblite.py module contributed by Ralf W. Grosse-Kunstleve.
Extended for Unicode by Steven Knight.

This is a very simple-minded "database" used for saved signature
information, with an interface modeled on the Python dbm database
interface module.
"""

import io
import os
import pickle
import shutil
import time

from SCons.compat import PICKLE_PROTOCOL

KEEP_ALL_FILES = False
IGNORE_CORRUPT_DBFILES = False


def corruption_warning(filename) -> None:
    """Local warning for corrupt db.

    Used for self-tests. SCons overwrites this with a
    different warning function in SConsign.py.
    """
    print("Warning: Discarding corrupt database:", filename)


DBLITE_SUFFIX = ".dblite"
TMP_SUFFIX = ".tmp"


class _Dblite:
    """Lightweight signature database class.

    Behaves like a dict when in memory, loads from a pickled disk
    file on open and writes back out to it on close.

    Open the database file using a path derived from *file_base_name*.
    The optional *flag* argument can be:

    +---------+---------------------------------------------------+
    | Value   | Meaning                                           |
    +=========+===================================================+
    | ``'r'`` | Open existing database for reading only (default) |
    +---------+---------------------------------------------------+
    | ``'w'`` | Open existing database for reading and  writing   |
    +---------+---------------------------------------------------+
    | ``'c'`` | Open database for reading and writing, creating   |
    |         | it if it doesn't exist                            |
    +---------+---------------------------------------------------+
    | ``'n'`` | Always create a new, empty database, open for     |
    |         | reading and writing                               |
    +---------+---------------------------------------------------+

    The optional *mode* argument is the POSIX mode of the file, used only
    when the database has to be created.  It defaults to octal ``0o666``.
    """

    # Because open() is defined at module level, overwriting builtin open
    # in the scope of this module, we use io.open to avoid ambiguity.
    _open = staticmethod(io.open)

    # we need to squirrel away references to functions from various modules
    # that we'll use when sync() is called: this may happen at Python
    # teardown time (we call it from our __del__), and the global module
    # references themselves may already have been rebound to None.
    _pickle_dump = staticmethod(pickle.dump)
    _pickle_protocol = PICKLE_PROTOCOL
    try:
        _os_chown = staticmethod(os.chown)
    except AttributeError:
        _os_chown = None
    _os_replace = staticmethod(os.replace)
    _os_chmod = staticmethod(os.chmod)
    _shutil_copyfile = staticmethod(shutil.copyfile)
    _time_time = staticmethod(time.time)

    def __init__(self, file_base_name, flag='r', mode=0o666) -> None:
        assert flag in ("r", "w", "c", "n")

        base, ext = os.path.splitext(file_base_name)
        if ext == DBLITE_SUFFIX:
            # There's already a suffix on the file name, don't add one.
            self._file_name = file_base_name
            self._tmp_name = base + TMP_SUFFIX
        else:
            self._file_name = file_base_name + DBLITE_SUFFIX
            self._tmp_name = file_base_name + TMP_SUFFIX

        self._flag = flag
        self._mode = mode
        self._dict = {}
        self._needs_sync = False

        if self._os_chown is not None and 0 in (os.geteuid(), os.getegid()):
            # running as root; chown back to current owner/group when done
            try:
                statinfo = os.stat(self._file_name)
                self._chown_to = statinfo.st_uid
                self._chgrp_to = statinfo.st_gid
            except OSError:
                # db file doesn't exist yet.
                # Check os.environ for SUDO_UID, use if set
                self._chown_to = int(os.environ.get('SUDO_UID', -1))
                self._chgrp_to = int(os.environ.get('SUDO_GID', -1))
        else:
            self._chown_to = -1  # don't chown
            self._chgrp_to = -1  # don't chgrp

        if self._flag == "n":
            with io.open(self._file_name, "wb", opener=self.opener):
                return  # just make sure it exists
        else:
            # We only need the disk file to slurp in the data.  Updates are
            # handled on close, db is mainained only in memory until then.
            try:
                with io.open(self._file_name, "rb") as f:
                    p = f.read()
            except OSError as e:
                # an error for file not to exist, unless flag is create
                if self._flag != "c":
                    raise e
                with io.open(self._file_name, "wb", opener=self.opener):
                    return  # just make sure it exists
            if len(p) > 0:
                try:
                    self._dict = pickle.loads(p, encoding='bytes')
                except (
                    pickle.UnpicklingError,
                    # Python3 docs:
                    # Note that other exceptions may also be raised during
                    # unpickling, including (but not necessarily limited to)
                    # AttributeError, EOFError, ImportError, and IndexError.
                    AttributeError,
                    EOFError,
                    ImportError,
                    IndexError,
                ):
                    if IGNORE_CORRUPT_DBFILES:
                        corruption_warning(self._file_name)
                    else:
                        raise

    def opener(self, path, flags):
        """Database open helper when creation may be needed.

        The high-level Python open() function cannot specify a file mode
        for creation. Using this as the opener with the saved mode lets
        us do that.
        """
        return os.open(path, flags, mode=self._mode)

    def close(self) -> None:
        if self._needs_sync:
            self.sync()

    def __del__(self) -> None:
        self.close()

    def sync(self) -> None:
        """Flush the database to disk.

        This routine *must* succeed, since the in-memory and on-disk
        copies are out of sync as soon as we do anything that changes
        the in-memory version. Thus, to be cautious, flush to a
        temporary file and then move it over with some error handling.
        """
        self._check_writable()
        with self._open(self._tmp_name, "wb", opener=self.opener) as f:
            self._pickle_dump(self._dict, f, self._pickle_protocol)

        try:
            self._os_replace(self._tmp_name, self._file_name)
        except PermissionError:
            # If we couldn't replace due to perms, try to change and retry.
            # This is mainly for Windows - on POSIX the file permissions
            # don't matter, the os.replace would have worked anyway.
            # We're giving up if the retry fails, just let the Python
            # exception abort us.
            try:
                self._os_chmod(self._file_name, 0o777)
            except PermissionError:
                pass
            self._os_replace(self._tmp_name, self._file_name)

        if (
            self._os_chown is not None and self._chown_to > 0
        ):  # don't chown to root or -1
            try:
                self._os_chown(self._file_name, self._chown_to, self._chgrp_to)
            except OSError:
                pass

        self._needs_sync = False
        if KEEP_ALL_FILES:
            self._shutil_copyfile(
                self._file_name, f"{self._file_name}_{int(self._time_time())}"
            )

    def _check_writable(self):
        if self._flag == "r":
            raise OSError(f"Read-only database: {self._file_name}")

    def __getitem__(self, key):
        return self._dict[key]

    def __setitem__(self, key, value):
        self._check_writable()

        if not isinstance(key, str):
            raise TypeError(f"key `{key}' must be a string but is {type(key)}")

        if not isinstance(value, bytes):
            raise TypeError(f"value `{value}' must be bytes but is {type(value)}")

        self._dict[key] = value
        self._needs_sync = True

    def __delitem__(self, key):
        del self._dict[key]

    def keys(self):
        return self._dict.keys()

    def items(self):
        return self._dict.items()

    def values(self):
        return self._dict.values()

    __iter__ = keys

    def __contains__(self, key) -> bool:
        return key in self._dict

    def __len__(self) -> int:
        return len(self._dict)


def open(file, flag="r", mode: int = 0o666):  # pylint: disable=redefined-builtin
    return _Dblite(file, flag, mode)


def _exercise():
    db = open("tmp", "n")
    assert len(db) == 0
    db["foo"] = b"bar"
    assert db["foo"] == b"bar"
    db.sync()

    db = open("tmp", "c")
    assert len(db) == 1, len(db)
    assert db["foo"] == b"bar"
    db["bar"] = b"foo"
    assert db["bar"] == b"foo"
    db.sync()

    db = open("tmp")
    assert len(db) == 2, len(db)
    assert db["foo"] == b"bar"
    assert db["bar"] == b"foo"
    try:
        db.sync()
    except OSError as e:
        assert str(e) == "Read-only database: tmp.dblite"
    else:
        raise RuntimeError("IOError expected.")
    db = open("tmp", "w")
    assert len(db) == 2, len(db)
    db["ping"] = b"pong"
    db.sync()

    try:
        db[(1, 2)] = "tuple"
    except TypeError as e:
        assert str(e) == "key `(1, 2)' must be a string but is <class 'tuple'>", str(e)
    else:
        raise RuntimeError("TypeError exception expected")

    try:
        db["list"] = [1, 2]
    except TypeError as e:
        assert str(e) == "value `[1, 2]' must be bytes but is <class 'list'>", str(e)
    else:
        raise RuntimeError("TypeError exception expected")

    db = open("tmp")
    assert len(db) == 3, len(db)

    db = open("tmp", "n")
    assert len(db) == 0, len(db)
    _Dblite._open("tmp.dblite", "w")

    db = open("tmp")
    _Dblite._open("tmp.dblite", "w").write("x")
    try:
        db = open("tmp")
    except pickle.UnpicklingError:
        pass
    else:
        raise RuntimeError("pickle exception expected.")

    global IGNORE_CORRUPT_DBFILES
    IGNORE_CORRUPT_DBFILES = True
    db = open("tmp")
    assert len(db) == 0, len(db)
    os.unlink("tmp.dblite")
    try:
        db = open("tmp", "w")
    except OSError as e:
        assert str(e) == "[Errno 2] No such file or directory: 'tmp.dblite'", str(e)
    else:
        raise RuntimeError("IOError expected.")

    print("Completed _exercise()")


if __name__ == "__main__":
    _exercise()

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
