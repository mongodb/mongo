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
SCons utility functions

This package contains routines for use by other parts of SCons.
Candidates for inclusion here are routines that do not need other parts
of SCons (other than Util), and have a reasonable chance of being useful
in multiple places, rather then being topical only to one module/package.
"""

# Warning: SCons.Util may not be able to import other parts of SCons
# globally without hitting import loops, as various modules import
# SCons.Util themselves. If a top-level import fails, try a local import.
# If local imports work, please annotate them for pylint (and for human
# readers) to know why, with:
#   importstuff  # pylint: disable=import-outside-toplevel
#
# Be aware that Black will break this if the annotated line is too long -
# which it almost certainly will be. It will split it like this:
#       from SCons.Errors import (
#           SConsEnvironmentError,
#       )  # pylint: disable=import-outside-toplevel
# That's syntactically valid as far as Python goes, but pylint will not
# recorgnize the annotation comment unless it's on the first line, like:
#       from SCons.Errors import (  # pylint: disable=import-outside-toplevel
#           SConsEnvironmentError,
#       )
# (issue filed on this upstream, for now just be aware)

from __future__ import annotations

import copy
import hashlib
import logging
import os
import re
import sys
import time
from collections import UserDict, UserList, deque
from contextlib import suppress
from types import MethodType, FunctionType
from typing import Any
from logging import Formatter

# Util split into a package. Make sure things that used to work
# when importing just Util itself still work:
from .sctypes import (
    DictTypes,
    ListTypes,
    SequenceTypes,
    StringTypes,
    BaseStringTypes,
    Null,
    NullSeq,
    is_Dict,
    is_List,
    is_Sequence,
    is_Tuple,
    is_String,
    is_Scalar,
    to_String,
    to_String_for_subst,
    to_String_for_signature,
    to_Text,
    to_bytes,
    to_str,
    get_env_bool,
    get_os_env_bool,
    get_environment_var,
)
from .hashes import (
    ALLOWED_HASH_FORMATS,
    DEFAULT_HASH_FORMATS,
    get_hash_format,
    set_hash_format,
    get_current_hash_algorithm_used,
    hash_signature,
    hash_file_signature,
    hash_collect,
    MD5signature,
    MD5filesignature,
    MD5collect,
)
from .envs import (
    MethodWrapper,
    PrependPath,
    AppendPath,
    AddPathIfNotExists,
    AddMethod,
    is_valid_construction_var,
)
from .filelock import FileLock, SConsLockFailure

PYPY = hasattr(sys, 'pypy_translation_info')

# this string will be hashed if a Node refers to a file that doesn't exist
# in order to distinguish from a file that exists but is empty.
NOFILE = "SCONS_MAGIC_MISSING_FILE_STRING"

# unused?
def dictify(keys, values, result=None) -> dict:
    if result is None:
        result = {}
    result.update(zip(keys, values))
    return result

_ALTSEP = os.altsep
if _ALTSEP is None and sys.platform == 'win32':
    # My ActivePython 2.0.1 doesn't set os.altsep!  What gives?
    _ALTSEP = '/'
if _ALTSEP:
    def rightmost_separator(path, sep):
        return max(path.rfind(sep), path.rfind(_ALTSEP))
else:
    def rightmost_separator(path, sep):
        return path.rfind(sep)

# First two from the Python Cookbook, just for completeness.
# (Yeah, yeah, YAGNI...)
def containsAny(s, pat) -> bool:
    """Check whether string `s` contains ANY of the items in `pat`."""
    return any(c in s for c in pat)

def containsAll(s, pat) -> bool:
    """Check whether string `s` contains ALL of the items in `pat`."""
    return all(c in s for c in pat)

def containsOnly(s, pat) -> bool:
    """Check whether string `s` contains ONLY items in `pat`."""
    for c in s:
        if c not in pat:
            return False
    return True


# TODO: Verify this method is STILL faster than os.path.splitext
def splitext(path) -> tuple:
    """Split `path` into a (root, ext) pair.

    Same as :mod:`os.path.splitext` but faster.
    """
    sep = rightmost_separator(path, os.sep)
    dot = path.rfind('.')
    # An ext is only real if it has at least one non-digit char
    if dot > sep and not path[dot + 1:].isdigit():
        return path[:dot], path[dot:]

    return path, ""

def updrive(path) -> str:
    """Make the drive letter (if any) upper case.

    This is useful because Windows is inconsistent on the case
    of the drive letter, which can cause inconsistencies when
    calculating command signatures.
    """
    drive, rest = os.path.splitdrive(path)
    if drive:
        path = drive.upper() + rest
    return path

class NodeList(UserList):
    """A list of Nodes with special attribute retrieval.

    Unlike an ordinary list, access to a member's attribute returns a
    `NodeList` containing the same attribute for each member.  Although
    this can hold any object, it is intended for use when processing
    Nodes, where fetching an attribute of each member is very commone,
    for example getting the content signature of each node.  The term
    "attribute" here includes the string representation.

    >>> someList = NodeList(['  foo  ', '  bar  '])
    >>> someList.strip()
    ['foo', 'bar']
    """

    def __bool__(self) -> bool:
        return bool(self.data)

    def __str__(self) -> str:
        return ' '.join(map(str, self.data))

    def __iter__(self):
        return iter(self.data)

    def __call__(self, *args, **kwargs) -> NodeList:
        result = [x(*args, **kwargs) for x in self.data]
        return self.__class__(result)

    def __getattr__(self, name) -> NodeList:
        """Returns a NodeList of `name` from each member."""
        result = [getattr(x, name) for x in self.data]
        return self.__class__(result)

    def __getitem__(self, index):
        """Returns one item, forces a `NodeList` if `index` is a slice."""
        # TODO: annotate return how? Union[] - don't know type of single item
        if isinstance(index, slice):
            return self.__class__(self.data[index])
        return self.data[index]


class DisplayEngine:
    """A callable class used to display SCons messages."""

    print_it = True

    def __call__(self, text, append_newline: int=1) -> None:
        if not self.print_it:
            return

        if append_newline:
            text = text + '\n'

        # Stdout might be connected to a pipe that has been closed
        # by now. The most likely reason for the pipe being closed
        # is that the user has press ctrl-c. It this is the case,
        # then SCons is currently shutdown. We therefore ignore
        # IOError's here so that SCons can continue and shutdown
        # properly so that the .sconsign is correctly written
        # before SCons exits.
        with suppress(IOError):
            sys.stdout.write(str(text))

    def set_mode(self, mode) -> None:
        self.print_it = mode

display = DisplayEngine()


# TODO: check if this could cause problems
# pylint: disable=dangerous-default-value
def render_tree(
    root,
    child_func,
    prune: bool = False,
    margin: list[bool] = [False],
    visited: dict | None = None,
) -> str:
    """Render a tree of nodes into an ASCII tree view.

    Args:
        root: the root node of the tree
        child_func: the function called to get the children of a node
        prune: don't visit the same node twice
        margin: the format of the left margin to use for children of *root*.
          Each entry represents a column where a true value will display
          a vertical bar and a false one a blank.
        visited: a dictionary of visited nodes in the current branch if
          *prune* is false, or in the whole tree if *prune* is true.
    """
    rname = str(root)

    # Initialize 'visited' dict, if required
    if visited is None:
        visited = {}

    children = child_func(root)
    retval = ""
    for pipe in margin[:-1]:
        if pipe:
            retval = retval + "| "
        else:
            retval = retval + "  "

    if rname in visited:
        return retval + "+-[" + rname + "]\n"

    retval = retval + "+-" + rname + "\n"
    if not prune:
        visited = copy.copy(visited)
    visited[rname] = True

    for i, child in enumerate(children):
        margin.append(i < len(children) - 1)
        retval = retval + render_tree(child, child_func, prune, margin, visited)
        margin.pop()

    return retval

def IDX(n) -> bool:
    """Generate in index into strings from the tree legends.

    These are always a choice between two, so bool works fine.
    """
    return bool(n)

# unicode line drawing chars:
BOX_HORIZ = chr(0x2500)  # '─'
BOX_VERT = chr(0x2502)  # '│'
BOX_UP_RIGHT = chr(0x2514)  # '└'
BOX_DOWN_RIGHT = chr(0x250c)  # '┌'
BOX_DOWN_LEFT = chr(0x2510)   # '┐'
BOX_UP_LEFT = chr(0x2518)  # '┘'
BOX_VERT_RIGHT = chr(0x251c)  # '├'
BOX_HORIZ_DOWN = chr(0x252c)  # '┬'


# TODO: check if this could cause problems
# pylint: disable=dangerous-default-value
def print_tree(
    root,
    child_func,
    prune: bool = False,
    showtags: int = 0,
    margin: list[bool] = [False],
    visited: dict | None = None,
    lastChild: bool = False,
    singleLineDraw: bool = False,
) -> None:
    """Print a tree of nodes.

    This is like func:`render_tree`, except it prints lines directly instead
    of creating a string representation in memory, so that huge trees can
    be handled.

    Args:
        root: the root node of the tree
        child_func: the function called to get the children of a node
        prune: don't visit the same node twice
        showtags: print status information to the left of each node line
            The default is false (value 0). A value of 2 will also print
            a legend for the margin tags.
        margin: the format of the left margin to use for children of *root*.
          Each entry represents a column, where a true value will display
          a vertical bar and a false one a blank.
        visited: a dictionary of visited nodes in the current branch if
          *prune* is false, or in the whole tree if *prune* is true.
        lastChild: this is the last leaf of a branch
        singleLineDraw: use line-drawing characters rather than ASCII.
    """

    rname = str(root)

    # Initialize 'visited' dict, if required
    if visited is None:
        visited = {}

    if showtags:

        if showtags == 2:
            legend = (' E         = exists\n' +
                      '  R        = exists in repository only\n' +
                      '   b       = implicit builder\n' +
                      '   B       = explicit builder\n' +
                      '    S      = side effect\n' +
                      '     P     = precious\n' +
                      '      A    = always build\n' +
                      '       C   = current\n' +
                      '        N  = no clean\n' +
                      '         H = no cache\n' +
                      '\n')
            sys.stdout.write(legend)

        tags = [
            '[',
            ' E'[IDX(root.exists())],
            ' R'[IDX(root.rexists() and not root.exists())],
            ' BbB'[
                [0, 1][IDX(root.has_explicit_builder())] +
                [0, 2][IDX(root.has_builder())]
            ],
            ' S'[IDX(root.side_effect)],
            ' P'[IDX(root.precious)],
            ' A'[IDX(root.always_build)],
            ' C'[IDX(root.is_up_to_date())],
            ' N'[IDX(root.noclean)],
            ' H'[IDX(root.nocache)],
            ']'
        ]

    else:
        tags = []

    def MMM(m):
        if singleLineDraw:
            return ["  ", BOX_VERT + " "][m]

        return ["  ", "| "][m]

    margins = list(map(MMM, margin[:-1]))
    children = child_func(root)
    cross = "+-"
    if singleLineDraw:
        cross = BOX_VERT_RIGHT + BOX_HORIZ   # sign used to point to the leaf.
        # check if this is the last leaf of the branch
        if lastChild:
            # if this if the last leaf, then terminate:
            cross = BOX_UP_RIGHT + BOX_HORIZ  # sign for the last leaf

        # if this branch has children then split it
        if children:
            # if it's a leaf:
            if prune and rname in visited and children:
                cross += BOX_HORIZ
            else:
                cross += BOX_HORIZ_DOWN

    if prune and rname in visited and children:
        sys.stdout.write(''.join(tags + margins + [cross, '[', rname, ']']) + '\n')
        return

    sys.stdout.write(''.join(tags + margins + [cross, rname]) + '\n')

    visited[rname] = 1

    # if this item has children:
    if children:
        margin.append(True)  # Initialize margin for vertical bar.
        idx = IDX(showtags)
        _child = 0  # Initialize this for the first child.
        for C in children[:-1]:
            _child = _child + 1  # number the children
            print_tree(
                C,
                child_func,
                prune,
                idx,
                margin,
                visited,
                (len(children) - _child) <= 0,
                singleLineDraw,
            )
        # margins are with space (index 0) because we arrived to the last child.
        margin[-1] = False
        # for this call child and nr of children needs to be set 0, to signal the second phase.
        print_tree(children[-1], child_func, prune, idx, margin, visited, True, singleLineDraw)
        margin.pop()  # destroy the last margin added


def do_flatten(  # pylint: disable=redefined-outer-name,redefined-builtin
    sequence,
    result,
    isinstance=isinstance,
    StringTypes=StringTypes,
    SequenceTypes=SequenceTypes,
) -> None:
    for item in sequence:
        if isinstance(item, StringTypes) or not isinstance(item, SequenceTypes):
            result.append(item)
        else:
            do_flatten(item, result)


def flatten(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj,
    isinstance=isinstance,
    StringTypes=StringTypes,
    SequenceTypes=SequenceTypes,
    do_flatten=do_flatten,
) -> list:
    """Flatten a sequence to a non-nested list.

    Converts either a single scalar or a nested sequence to a non-nested list.
    Note that :func:`flatten` considers strings
    to be scalars instead of sequences like pure Python would.
    """
    if isinstance(obj, StringTypes) or not isinstance(obj, SequenceTypes):
        return [obj]
    result = []
    for item in obj:
        if isinstance(item, StringTypes) or not isinstance(item, SequenceTypes):
            result.append(item)
        else:
            do_flatten(item, result)
    return result


def flatten_sequence(  # pylint: disable=redefined-outer-name,redefined-builtin
    sequence,
    isinstance=isinstance,
    StringTypes=StringTypes,
    SequenceTypes=SequenceTypes,
    do_flatten=do_flatten,
) -> list:
    """Flatten a sequence to a non-nested list.

    Same as :func:`flatten`, but it does not handle the single scalar case.
    This is slightly more efficient when one knows that the sequence
    to flatten can not be a scalar.
    """
    result = []
    for item in sequence:
        if isinstance(item, StringTypes) or not isinstance(item, SequenceTypes):
            result.append(item)
        else:
            do_flatten(item, result)
    return result


# The SCons "semi-deep" copy.
#
# This makes separate copies of lists (including UserList objects)
# dictionaries (including UserDict objects) and tuples, but just copies
# references to anything else it finds.
#
# A special case is any object that has a __semi_deepcopy__() method,
# which we invoke to create the copy. Currently only used by
# BuilderDict to actually prevent the copy operation (as invalid on that object).
#
# The dispatch table approach used here is a direct rip-off from the
# normal Python copy module.

def semi_deepcopy_dict(obj, exclude=None) -> dict:
    if exclude is None:
        exclude = []
    return {k: semi_deepcopy(v) for k, v in obj.items() if k not in exclude}

def _semi_deepcopy_list(obj) -> list:
    return [semi_deepcopy(item) for item in obj]

def _semi_deepcopy_tuple(obj) -> tuple:
    return tuple(map(semi_deepcopy, obj))

_semi_deepcopy_dispatch = {
    dict: semi_deepcopy_dict,
    list: _semi_deepcopy_list,
    tuple: _semi_deepcopy_tuple,
}


def semi_deepcopy(obj):
    copier = _semi_deepcopy_dispatch.get(type(obj))
    if copier:
        return copier(obj)

    if hasattr(obj, '__semi_deepcopy__') and callable(obj.__semi_deepcopy__):
        return obj.__semi_deepcopy__()

    if isinstance(obj, UserDict):
        return obj.__class__(semi_deepcopy_dict(obj))

    if isinstance(obj, (UserList, deque)):
        return obj.__class__(_semi_deepcopy_list(obj))

    return obj


class Proxy:
    """A simple generic Proxy class, forwarding all calls to subject.

    This means you can take an object, let's call it `'obj_a``,
    and wrap it in this Proxy class, with a statement like this::

        proxy_obj = Proxy(obj_a)

    Then, if in the future, you do something like this::

        x = proxy_obj.var1

    since the :class:`Proxy` class does not have a :attr:`var1` attribute
    (but presumably ``obj_a`` does), the request actually is equivalent
    to saying::

        x = obj_a.var1

    Inherit from this class to create a Proxy.

    With Python 3.5+ this does *not* work transparently
    for :class:`Proxy` subclasses that use special dunder method names,
    because those names are now bound to the class, not the individual
    instances.  You now need to know in advance which special method names you
    want to pass on to the underlying Proxy object, and specifically delegate
    their calls like this::

        class Foo(Proxy):
            __str__ = Delegate('__str__')
    """

    def __init__(self, subject) -> None:
        """Wrap an object as a Proxy object"""
        self._subject = subject

    def __getattr__(self, name):
        """Retrieve an attribute from the wrapped object.

        Raises:
           AttributeError: if attribute `name` doesn't exist.
        """
        return getattr(self._subject, name)

    def get(self):
        """Retrieve the entire wrapped object"""
        return self._subject

    def __eq__(self, other):
        if issubclass(other.__class__, self._subject.__class__):
            return self._subject == other
        return self.__dict__ == other.__dict__


class Delegate:
    """A Python Descriptor class that delegates attribute fetches
    to an underlying wrapped subject of a Proxy.  Typical use::

        class Foo(Proxy):
            __str__ = Delegate('__str__')
    """
    def __init__(self, attribute) -> None:
        self.attribute = attribute

    def __get__(self, obj, cls):
        if isinstance(obj, cls):
            return getattr(obj._subject, self.attribute)

        return self


# attempt to load the windows registry module:
can_read_reg = False
try:
    import winreg

    can_read_reg = True
    hkey_mod = winreg

except ImportError:
    class _NoError(Exception):
        pass
    RegError = _NoError

if can_read_reg:
    HKEY_CLASSES_ROOT = hkey_mod.HKEY_CLASSES_ROOT
    HKEY_LOCAL_MACHINE = hkey_mod.HKEY_LOCAL_MACHINE
    HKEY_CURRENT_USER = hkey_mod.HKEY_CURRENT_USER
    HKEY_USERS = hkey_mod.HKEY_USERS

    RegOpenKeyEx = winreg.OpenKeyEx
    RegEnumKey = winreg.EnumKey
    RegEnumValue = winreg.EnumValue
    RegQueryValueEx = winreg.QueryValueEx
    RegError = winreg.error

    def RegGetValue(root, key):
        r"""Returns a registry value without having to open the key first.

        Only available on Windows platforms with a version of Python that
        can read the registry.

        Returns the same thing as :func:`RegQueryValueEx`, except you just
        specify the entire path to the value, and don't have to bother
        opening the key first.  So, instead of::

          k = SCons.Util.RegOpenKeyEx(SCons.Util.HKEY_LOCAL_MACHINE,
                r'SOFTWARE\Microsoft\Windows\CurrentVersion')
          out = SCons.Util.RegQueryValueEx(k, 'ProgramFilesDir')

        You can write::

          out = SCons.Util.RegGetValue(SCons.Util.HKEY_LOCAL_MACHINE,
                r'SOFTWARE\Microsoft\Windows\CurrentVersion\ProgramFilesDir')
        """
        # I would use os.path.split here, but it's not a filesystem
        # path...
        p = key.rfind('\\') + 1
        keyp = key[: p - 1]  # -1 to omit trailing slash
        val = key[p:]
        k = RegOpenKeyEx(root, keyp)
        return RegQueryValueEx(k, val)


else:
    HKEY_CLASSES_ROOT = None
    HKEY_LOCAL_MACHINE = None
    HKEY_CURRENT_USER = None
    HKEY_USERS = None

    def RegGetValue(root, key):
        raise OSError

    def RegOpenKeyEx(root, key):
        raise OSError


if sys.platform == 'win32':

    def WhereIs(file, path=None, pathext=None, reject=None) -> str | None:
        if path is None:
            try:
                path = os.environ['PATH']
            except KeyError:
                return None
        if is_String(path):
            path = path.split(os.pathsep)
        if pathext is None:
            try:
                pathext = os.environ['PATHEXT']
            except KeyError:
                pathext = '.COM;.EXE;.BAT;.CMD'
        if is_String(pathext):
            pathext = pathext.split(os.pathsep)
        for ext in pathext:
            if ext.lower() == file[-len(ext):].lower():
                pathext = ['']
                break
        if reject is None:
            reject = []
        if not is_List(reject) and not is_Tuple(reject):
            reject = [reject]
        for p in path:
            f = os.path.join(p, file)
            for ext in pathext:
                fext = f + ext
                if os.path.isfile(fext):
                    try:
                        reject.index(fext)
                    except ValueError:
                        return os.path.normpath(fext)
                    continue
        return None

elif os.name == 'os2':

    def WhereIs(file, path=None, pathext=None, reject=None) -> str | None:
        if path is None:
            try:
                path = os.environ['PATH']
            except KeyError:
                return None
        if is_String(path):
            path = path.split(os.pathsep)
        if pathext is None:
            pathext = ['.exe', '.cmd']
        for ext in pathext:
            if ext.lower() == file[-len(ext):].lower():
                pathext = ['']
                break
        if reject is None:
            reject = []
        if not is_List(reject) and not is_Tuple(reject):
            reject = [reject]
        for p in path:
            f = os.path.join(p, file)
            for ext in pathext:
                fext = f + ext
                if os.path.isfile(fext):
                    try:
                        reject.index(fext)
                    except ValueError:
                        return os.path.normpath(fext)
                    continue
        return None

else:

    def WhereIs(file, path=None, pathext=None, reject=None) -> str | None:
        import stat  # pylint: disable=import-outside-toplevel

        if path is None:
            try:
                path = os.environ['PATH']
            except KeyError:
                return None
        if is_String(path):
            path = path.split(os.pathsep)
        if reject is None:
            reject = []
        if not is_List(reject) and not is_Tuple(reject):
            reject = [reject]
        for p in path:
            f = os.path.join(p, file)
            if os.path.isfile(f):
                try:
                    mode = os.stat(f).st_mode
                except OSError:
                    # os.stat() raises OSError, not IOError if the file
                    # doesn't exist, so in this case we let IOError get
                    # raised so as to not mask possibly serious disk or
                    # network issues.
                    continue
                if stat.S_IXUSR & mode:
                    try:
                        reject.index(f)
                    except ValueError:
                        return os.path.normpath(f)
                    continue
        return None

WhereIs.__doc__ = """\
Return the path to an executable that matches *file*.

Searches the given *path* for *file*, considering any filename
extensions in *pathext* (on the Windows platform only), and
returns the full path to the matching command of the first match,
or ``None`` if there are no matches.
Will not select any path name or names in the optional
*reject* list.

If *path* is ``None`` (the default), :attr:`os.environ[PATH]` is used.
On Windows, If *pathext* is ``None`` (the default),
:attr:`os.environ[PATHEXT]` is used.

The construction environment method of the same name wraps a
call to this function by filling in *path* from the execution
environment if it is ``None`` (and for *pathext* on Windows,
if necessary), so if called from there, this function
will not backfill from :attr:`os.environ`.

Note:
   Finding things in :attr:`os.environ` may answer the question
   "does *file* exist on the system", but not the question
   "can SCons use that executable", unless the path element that
   yields the match is also in the the Execution Environment
   (e.g. ``env['ENV']['PATH']``). Since this utility function has no
   environment reference, it cannot make that determination.
"""


if sys.platform == 'cygwin':
    import subprocess  # pylint: disable=import-outside-toplevel

    def get_native_path(path: str) -> str:
        cp = subprocess.run(('cygpath', '-w', path), check=False, stdout=subprocess.PIPE)
        return cp.stdout.decode().replace('\n', '')
else:
    def get_native_path(path: str) -> str:
        return path

get_native_path.__doc__ = """\
Transform an absolute path into a native path for the system.

In Cygwin, this converts from a Cygwin path to a Windows path,
without regard to whether *path* refers to an existing file
system object.  For other platforms, *path* is unchanged.
"""


def Split(arg) -> list:
    """Returns a list of file names or other objects.

    If *arg* is a string, it will be split on whitespace
    within the string.  If *arg* is already a list, the list
    will be returned untouched. If *arg* is any other type of object,
    it will be returned in a single-item list.

    >>> print(Split(" this  is  a  string  "))
    ['this', 'is', 'a', 'string']
    >>> print(Split(["stringlist", " preserving ", " spaces "]))
    ['stringlist', ' preserving ', ' spaces ']
    """
    if is_List(arg) or is_Tuple(arg):
        return arg

    if is_String(arg):
        return arg.split()

    return [arg]


class CLVar(UserList):
    """A container for command-line construction variables.

    Forces the use of a list of strings intended as command-line
    arguments.  Like :class:`collections.UserList`, but the argument
    passed to the initializter will be processed by the :func:`Split`
    function, which includes special handling for string types: they
    will be split into a list of words, not coereced directly to a list.
    The same happens if a string is added to a :class:`CLVar`,
    which allows doing the right thing with both
    :func:`Append`/:func:`Prepend` methods,
    as well as with pure Python addition, regardless of whether adding
    a list or a string to a construction variable.

    Side effect: spaces will be stripped from individual string
    arguments. If you need spaces preserved, pass strings containing
    spaces inside a list argument.

    >>> u = UserList("--some --opts and args")
    >>> print(len(u), repr(u))
    22 ['-', '-', 's', 'o', 'm', 'e', ' ', '-', '-', 'o', 'p', 't', 's', ' ', 'a', 'n', 'd', ' ', 'a', 'r', 'g', 's']
    >>> c = CLVar("--some --opts and args")
    >>> print(len(c), repr(c))
    4 ['--some', '--opts', 'and', 'args']
    >>> c += "   strips spaces   "
    >>> print(len(c), repr(c))
    6 ['--some', '--opts', 'and', 'args', 'strips', 'spaces']
    >>> c += ["   does not split or strip   "]
    7 ['--some', '--opts', 'and', 'args', 'strips', 'spaces', '   does not split or strip   ']
    """

    def __init__(self, initlist=None) -> None:
        super().__init__(Split(initlist if initlist is not None else []))

    def __add__(self, other):
        return super().__add__(CLVar(other))

    def __radd__(self, other):
        return super().__radd__(CLVar(other))

    def __iadd__(self, other):
        return super().__iadd__(CLVar(other))

    def __str__(self) -> str:
        # Some cases the data can contain Nodes, so make sure they
        # processed to string before handing them over to join.
        return ' '.join([str(d) for d in self.data])


class Selector(dict):
    """A callable dict for file suffix lookup.

    Often used to associate actions or emitters with file types.

    Depends on insertion order being preserved so that :meth:`get_suffix`
    calls always return the first suffix added.
    """
    def __call__(self, env, source, ext=None):
        if ext is None:
            try:
                ext = source[0].get_suffix()
            except IndexError:
                ext = ""
        try:
            return self[ext]
        except KeyError as exc:
            # Try to perform Environment substitution on the keys of
            # the dictionary before giving up.
            s_dict = {}
            for (k, v) in self.items():
                if k is not None:
                    s_k = env.subst(k)
                    if s_k in s_dict:
                        # We only raise an error when variables point
                        # to the same suffix.  If one suffix is literal
                        # and a variable suffix contains this literal,
                        # the literal wins and we don't raise an error.
                        raise KeyError(s_dict[s_k][0], k, s_k) from exc
                    s_dict[s_k] = (k, v)
            try:
                return s_dict[ext][1]
            except KeyError:
                try:
                    return self[None]
                except KeyError:
                    return None


if sys.platform == 'cygwin':
    # On Cygwin, os.path.normcase() lies, so just report back the
    # fact that the underlying Windows OS is case-insensitive.
    def case_sensitive_suffixes(s1: str, s2: str) -> bool:  # pylint: disable=unused-argument
        return False

else:
    def case_sensitive_suffixes(s1: str, s2: str) -> bool:
        return os.path.normcase(s1) != os.path.normcase(s2)

case_sensitive_suffixes.__doc__ = """\
Returns whether platform distinguishes case in file suffixes."""


def adjustixes(fname, pre, suf, ensure_suffix: bool=False) -> str:
    """Adjust filename prefixes and suffixes as needed.

    Add `prefix` to `fname` if specified.
    Add `suffix` to `fname` if specified and if `ensure_suffix` is ``True``
    """

    if pre:
        path, fn = os.path.split(os.path.normpath(fname))

        # Handle the odd case where the filename = the prefix.
        # In that case, we still want to add the prefix to the file
        if not fn.startswith(pre) or fn == pre:
            fname = os.path.join(path, pre + fn)
    # Only append a suffix if the suffix we're going to add isn't already
    # there, and if either we've been asked to ensure the specific suffix
    # is present or there's no suffix on it at all.
    # Also handle the odd case where the filename = the suffix.
    # in that case we still want to append the suffix
    if suf and not fname.endswith(suf) and \
            (ensure_suffix or not splitext(fname)[1]):
        fname = fname + suf
    return fname


# From Tim Peters,
# https://code.activestate.com/recipes/52560
# ASPN: Python Cookbook: Remove duplicates from a sequence
# (Also in the printed Python Cookbook.)
# Updated. This algorithm is used by some scanners and tools.

def unique(seq):
    """Return a list of the elements in seq without duplicates, ignoring order.

    For best speed, all sequence elements should be hashable.  Then
    :func:`unique` will usually work in linear time.

    If not possible, the sequence elements should enjoy a total
    ordering, and if ``list(s).sort()`` doesn't raise ``TypeError``
    it is assumed that they do enjoy a total ordering.  Then
    :func:`unique` will usually work in O(N*log2(N)) time.

    If that's not possible either, the sequence elements must support
    equality-testing.  Then :func:`unique` will usually work in quadratic time.

    >>> mylist = unique([1, 2, 3, 1, 2, 3])
    >>> print(sorted(mylist))
    [1, 2, 3]
    >>> mylist = unique("abcabc")
    >>> print(sorted(mylist))
    ['a', 'b', 'c']
    >>> mylist = unique(([1, 2], [2, 3], [1, 2]))
    >>> print(sorted(mylist))
    [[1, 2], [2, 3]]
    """

    if not seq:
        return []

    # Try using a dict first, as that's the fastest and will usually
    # work.  If it doesn't work, it will usually fail quickly, so it
    # usually doesn't cost much to *try* it.  It requires that all the
    # sequence elements be hashable, and support equality comparison.
    # TODO: should be even faster: return(list(set(seq)))
    with suppress(TypeError):
        return list(dict.fromkeys(seq))

    # We couldn't hash all the elements (got a TypeError).
    # Next fastest is to sort, which brings the equal elements together;
    # then duplicates are easy to weed out in a single pass.
    # NOTE:  Python's list.sort() was designed to be efficient in the
    # presence of many duplicate elements.  This isn't true of all
    # sort functions in all languages or libraries, so this approach
    # is more effective in Python than it may be elsewhere.
    n = len(seq)
    try:
        t = sorted(seq)
    except TypeError:
        pass    # move on to the next method
    else:
        last = t[0]
        lasti = i = 1
        while i < n:
            if t[i] != last:
                t[lasti] = last = t[i]
                lasti = lasti + 1
            i = i + 1
        return t[:lasti]

    # Brute force is all that's left.
    u = []
    for x in seq:
        if x not in u:
            u.append(x)
    return u

# Best way (assuming Python 3.7, but effectively 3.6) to remove
# duplicates from a list in while preserving order, according to
# https://stackoverflow.com/questions/480214/how-do-i-remove-duplicates-from-a-list-while-preserving-order/17016257#17016257
def uniquer_hashables(seq):
    return list(dict.fromkeys(seq))

# Recipe 19.11 "Reading Lines with Continuation Characters",
# by Alex Martelli, straight from the Python CookBook (2nd edition).
def logical_lines(physical_lines, joiner=''.join):
    logical_line = []
    for line in physical_lines:
        stripped = line.rstrip()
        if stripped.endswith('\\'):
            # a line which continues w/the next physical line
            logical_line.append(stripped[:-1])
        else:
            # a line which does not continue, end of logical line
            logical_line.append(line)
            yield joiner(logical_line)
            logical_line = []
    if logical_line:
        # end of sequence implies end of last logical line
        yield joiner(logical_line)


class LogicalLines:
    """Wrapper class for the :func:`logical_lines` function.

    Allows us to read all "logical" lines at once from a given file object.
    """

    def __init__(self, fileobj) -> None:
        self.fileobj = fileobj

    def readlines(self):
        return list(logical_lines(self.fileobj))


class UniqueList(UserList):
    """A list which maintains uniqueness.

    Uniquing is lazy: rather than being enforced on list changes, it is fixed
    up on access by those methods which need to act on a unique list to be
    correct. That means things like membership tests don't have to eat the
    uniquing time.
    """
    def __init__(self, initlist=None) -> None:
        super().__init__(initlist)
        self.unique = True

    def __make_unique(self) -> None:
        if not self.unique:
            self.data = uniquer_hashables(self.data)
            self.unique = True

    def __repr__(self) -> str:
        self.__make_unique()
        return super().__repr__()

    def __lt__(self, other):
        self.__make_unique()
        return super().__lt__(other)

    def __le__(self, other):
        self.__make_unique()
        return super().__le__(other)

    def __eq__(self, other):
        self.__make_unique()
        return super().__eq__(other)

    def __ne__(self, other):
        self.__make_unique()
        return super().__ne__(other)

    def __gt__(self, other):
        self.__make_unique()
        return super().__gt__(other)

    def __ge__(self, other):
        self.__make_unique()
        return super().__ge__(other)

    # __contains__ doesn't need to worry about uniquing, inherit

    def __len__(self) -> int:
        self.__make_unique()
        return super().__len__()

    def __getitem__(self, i):
        self.__make_unique()
        return super().__getitem__(i)

    def __setitem__(self, i, item) -> None:
        super().__setitem__(i, item)
        self.unique = False

    # __delitem__ doesn't need to worry about uniquing, inherit

    def __add__(self, other):
        result = super().__add__(other)
        result.unique = False
        return result

    def __radd__(self, other):
        result = super().__radd__(other)
        result.unique = False
        return result

    def __iadd__(self, other):
        result = super().__iadd__(other)
        result.unique = False
        return result

    def __mul__(self, other):
        result = super().__mul__(other)
        result.unique = False
        return result

    def __rmul__(self, other):
        result = super().__rmul__(other)
        result.unique = False
        return result

    def __imul__(self, other):
        result = super().__imul__(other)
        result.unique = False
        return result

    def append(self, item) -> None:
        super().append(item)
        self.unique = False

    def insert(self, i, item) -> None:
        super().insert(i, item)
        self.unique = False

    def count(self, item):
        self.__make_unique()
        return super().count(item)

    def index(self, item, *args):
        self.__make_unique()
        return super().index(item, *args)

    def reverse(self) -> None:
        self.__make_unique()
        super().reverse()

    # TODO: Py3.8: def sort(self, /, *args, **kwds):
    def sort(self, *args, **kwds):
        self.__make_unique()
        return super().sort(*args, **kwds)

    def extend(self, other) -> None:
        super().extend(other)
        self.unique = False


class Unbuffered:
    """A proxy  that wraps a file object, flushing after every write.

    Delegates everything else to the wrapped object.
    """
    def __init__(self, file) -> None:
        self.file = file

    def write(self, arg) -> None:
        # Stdout might be connected to a pipe that has been closed
        # by now. The most likely reason for the pipe being closed
        # is that the user has press ctrl-c. It this is the case,
        # then SCons is currently shutdown. We therefore ignore
        # IOError's here so that SCons can continue and shutdown
        # properly so that the .sconsign is correctly written
        # before SCons exits.
        with suppress(IOError):
            self.file.write(arg)
            self.file.flush()

    def writelines(self, arg) -> None:
        with suppress(IOError):
            self.file.writelines(arg)
            self.file.flush()

    def __getattr__(self, attr):
        return getattr(self.file, attr)

def make_path_relative(path) -> str:
    """Converts an absolute path name to a relative pathname."""

    if os.path.isabs(path):
        drive_s, path = os.path.splitdrive(path)

        if not drive_s:
            path = re.compile(r"/*(.*)").findall(path)[0]
        else:
            path = path[1:]

    assert not os.path.isabs(path), path
    return path


def silent_intern(__string: Any) -> str:
    """Intern a string without failing.

    Perform :mod:`sys.intern` on the passed argument and return the result.
    If the input is ineligible for interning the original argument is
    returned and no exception is thrown.
    """
    try:
        return sys.intern(__string)
    except TypeError:
        return __string


def cmp(a, b) -> bool:
    """A cmp function because one is no longer available in Python3."""
    return (a > b) - (a < b)


def print_time():
    """Hack to return a value from Main if can't import Main."""
    # this specifically violates the rule of Util not depending on other
    # parts of SCons in order to work around other import-loop issues.
    #
    # pylint: disable=redefined-outer-name,import-outside-toplevel
    from SCons.Script.Main import print_time
    return print_time


def wait_for_process_to_die(pid: int) -> None:
    """
    Wait for the specified process to die.

    TODO: Add timeout which raises exception
    """
    # wait for the process to fully killed
    try:
        import psutil  # pylint: disable=import-outside-toplevel
        while True:
            # TODO: this should use psutil.process_exists() or psutil.Process.wait()
            # The psutil docs explicitly recommend against using process_iter()/pids()
            # for checking the existence of a process.
            if pid not in [proc.pid for proc in psutil.process_iter()]:
                break
            time.sleep(0.1)
    except ImportError:
        # if psutil is not installed we can do this the hard way
        _wait_for_process_to_die_non_psutil(pid, timeout=-1.0)


def _wait_for_process_to_die_non_psutil(pid: int, timeout: float = 60.0) -> None:
    start_time = time.time()
    while True:
        if not _is_process_alive(pid):
            break
        if timeout >= 0.0 and time.time() - start_time > timeout:
            raise TimeoutError(f"timed out waiting for process {pid}")
        time.sleep(0.1)


if sys.platform == 'win32':
    def _is_process_alive(pid: int) -> bool:
        import ctypes  # pylint: disable=import-outside-toplevel
        PROCESS_QUERY_INFORMATION = 0x1000
        STILL_ACTIVE = 259

        processHandle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_INFORMATION, 0, pid)
        if processHandle == 0:
            return False

        # OpenProcess() may successfully return a handle even for terminated
        # processes when something else in the system is still holding a
        # reference to their handle.  Call GetExitCodeProcess() to check if the
        # process has already exited.
        try:
            exit_code = ctypes.c_ulong()
            success = ctypes.windll.kernel32.GetExitCodeProcess(
                    processHandle, ctypes.byref(exit_code))
            if success:
                return exit_code.value == STILL_ACTIVE
        finally:
            ctypes.windll.kernel32.CloseHandle(processHandle)

        return True

else:
    def _is_process_alive(pid: int) -> bool:
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False


# From: https://stackoverflow.com/questions/1741972/how-to-use-different-formatters-with-the-same-logging-handler-in-python
class DispatchingFormatter(Formatter):
    """Logging formatter which dispatches to various formatters."""

    def __init__(self, formatters, default_formatter) -> None:
        self._formatters = formatters
        self._default_formatter = default_formatter

    def format(self, record):
        # Search from record's logger up to its parents:
        logger = logging.getLogger(record.name)
        while logger:
            # Check if suitable formatter for current logger exists:
            if logger.name in self._formatters:
                formatter = self._formatters[logger.name]
                break
            logger = logger.parent
        else:
            # If no formatter found, just use default:
            formatter = self._default_formatter
        return formatter.format(record)


def sanitize_shell_env(execution_env: dict) -> dict:
    """Sanitize all values in *execution_env*

    The execution environment (typically comes from ``env['ENV']``) is
    propagated to the shell, and may need to be cleaned first.

    Args:
        execution_env: The shell environment variables to be propagated
        to the spawned shell.

    Returns:
        sanitized dictionary of env variables (similar to what you'd get
        from :data:`os.environ`)
    """
    # Ensure that the ENV values are all strings:
    new_env = {}
    for key, value in execution_env.items():
        if is_List(value):
            # If the value is a list, then we assume it is a path list,
            # because that's a pretty common list-like value to stick
            # in an environment variable:
            value = flatten_sequence(value)
            new_env[key] = os.pathsep.join(map(str, value))
        else:
            # It's either a string or something else.  If it isn't a
            # string or a list, then we just coerce it to a string, which
            # is the proper way to handle Dir and File instances and will
            # produce something reasonable for just about everything else:
            new_env[key] = str(value)
    return new_env

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
