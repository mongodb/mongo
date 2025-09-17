# SPDX-License-Identifier: MIT
#
# Copyright The SCons Foundation

"""Various SCons utility functions

Routines which check types and do type conversions.
"""
from __future__ import annotations

import codecs
import os
import pprint
import re
import sys

from collections import UserDict, UserList, UserString, deque
from collections.abc import MappingView, Iterable

# Functions for deciding if things are like various types, mainly to
# handle UserDict, UserList and UserString like their underlying types.
#
# Yes, all of this manual testing breaks polymorphism, and the real
# Pythonic way to do all of this would be to just try it and handle the
# exception, but handling the exception when it's not the right type is
# often too slow.

# A trick is used to speed up these functions. Default arguments are
# used to take a snapshot of the global functions and constants used
# by these functions. This transforms accesses to global variables into
# local variable accesses (i.e. LOAD_FAST instead of LOAD_GLOBAL).
# Since checkers dislike this, it's now annotated for pylint, to flag
# (mostly for other readers of this code) we're doing this intentionally.
# TODO: experts affirm this is still faster, but maybe check if worth it?

DictTypes = (dict, UserDict)
ListTypes = (list, UserList, deque)

# With Python 3, there are view types that are sequences. Other interesting
# sequences are range and bytearray.  What we don't want is strings: while
# they are iterable sequences, in SCons usage iterating over a string is
# almost never what we want. So basically iterable-but-not-string:
SequenceTypes = (list, tuple, deque, UserList, MappingView)

# Note that profiling data shows a speed-up when comparing
# explicitly with str instead of simply comparing
# with basestring. (at least on Python 2.5.1)
# TODO: PY3 check this benchmarking is still correct.
StringTypes = (str, UserString)

# Empirically, it is faster to check explicitly for str than for basestring.
BaseStringTypes = str

# Later Python versions allow us to explicitly apply type hints based off the
# return value similar to isinstance(), albeit not as precise.
if sys.version_info >= (3, 13):
    from typing import TypeAlias, TypeIs

    DictTypeRet: TypeAlias = TypeIs[dict | UserDict]
    ListTypeRet: TypeAlias = TypeIs[list | UserList | deque]
    SequenceTypeRet: TypeAlias = TypeIs[list | tuple | deque | UserList | MappingView]
    TupleTypeRet: TypeAlias = TypeIs[tuple]
    StringTypeRet: TypeAlias = TypeIs[str | UserString]
elif sys.version_info >= (3, 10):
    from typing import TypeAlias, TypeGuard

    DictTypeRet: TypeAlias = TypeGuard[dict | UserDict]
    ListTypeRet: TypeAlias = TypeGuard[list | UserList | deque]
    SequenceTypeRet: TypeAlias = TypeGuard[list | tuple | deque | UserList | MappingView]
    TupleTypeRet: TypeAlias = TypeGuard[tuple]
    StringTypeRet: TypeAlias = TypeGuard[str | UserString]
else:
    # Because we have neither `TypeAlias` class nor `type` keyword pre-3.10,
    # the boolean fallback type has to be wrapped in the legacy `Union` class.
    from typing import Union

    DictTypeRet = Union[bool, bool]
    ListTypeRet = Union[bool, bool]
    SequenceTypeRet = Union[bool, bool]
    TupleTypeRet = Union[bool, bool]
    StringTypeRet = Union[bool, bool]


def is_Dict(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj, isinstance=isinstance, DictTypes=DictTypes
) -> DictTypeRet:
    """Check if object is a dict."""
    return isinstance(obj, DictTypes)


def is_List(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj, isinstance=isinstance, ListTypes=ListTypes
) -> ListTypeRet:
    """Check if object is a list."""
    return isinstance(obj, ListTypes)


def is_Sequence(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj, isinstance=isinstance, SequenceTypes=SequenceTypes
) -> SequenceTypeRet:
    """Check if object is a sequence."""
    return isinstance(obj, SequenceTypes)


def is_Tuple(  # pylint: disable=redefined-builtin
    obj, isinstance=isinstance, tuple=tuple
) -> TupleTypeRet:
    """Check if object is a tuple."""
    return isinstance(obj, tuple)


def is_String(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj, isinstance=isinstance, StringTypes=StringTypes
) -> StringTypeRet:
    """Check if object is a string."""
    return isinstance(obj, StringTypes)


def is_Scalar(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj, isinstance=isinstance, StringTypes=StringTypes, Iterable=Iterable,
) -> bool:
    """Check if object is a scalar: not a container or iterable."""
    # Profiling shows that there is an impressive speed-up of 2x
    # when explicitly checking for strings instead of just not
    # sequence when the argument (i.e. obj) is already a string.
    # But, if obj is a not string then it is twice as fast to
    # check only for 'not sequence'. The following code therefore
    # assumes that the obj argument is a string most of the time.
    # Update: now using collections.abc.Iterable for the 2nd check.
    # Note: None is considered a "scalar" for this check, which is correct
    # for the usage in SCons.Environment._add_cppdefines.
    return isinstance(obj, StringTypes) or not isinstance(obj, Iterable)


# From Dinu C. Gherman,
# Python Cookbook, second edition, recipe 6.17, p. 277.
# Also: https://code.activestate.com/recipes/68205
# ASPN: Python Cookbook: Null Object Design Pattern


class Null:
    """Null objects always and reliably 'do nothing'."""

    def __new__(cls, *args, **kwargs):
        if '_instance' not in vars(cls):
            cls._instance = super().__new__(cls, *args, **kwargs)
        return cls._instance

    def __init__(self, *args, **kwargs) -> None:
        pass

    def __call__(self, *args, **kwargs):
        return self

    def __repr__(self) -> str:
        return f"Null(0x{id(self):08X})"

    def __bool__(self) -> bool:
        return False

    def __getattr__(self, name):
        return self

    def __setattr__(self, name, value):
        return self

    def __delattr__(self, name):
        return self


class NullSeq(Null):
    """A Null object that can also be iterated over."""

    def __len__(self) -> int:
        return 0

    def __iter__(self):
        return iter(())

    def __getitem__(self, i):
        return self

    def __delitem__(self, i):
        return self

    def __setitem__(self, i, v):
        return self


def to_bytes(s) -> bytes:
    """Convert object to bytes."""
    if s is None:
        return b'None'
    if isinstance(s, (bytes, bytearray)):
        # if already bytes return.
        return s
    return bytes(s, 'utf-8')


def to_str(s) -> str:
    """Convert object to string."""
    if s is None:
        return 'None'
    if is_String(s):
        return s
    return str(s, 'utf-8')


# Generic convert-to-string functions.  The wrapper
# to_String_for_signature() will use a for_signature() method if the
# specified object has one.


def to_String(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj,
    isinstance=isinstance,
    str=str,
    UserString=UserString,
    BaseStringTypes=BaseStringTypes,
) -> str:
    """Return a string version of obj.

    Use this for data likely to be well-behaved. Use
    :func:`to_Text` for unknown file data that needs to be decoded.
    """
    if isinstance(obj, BaseStringTypes):
        # Early out when already a string!
        return obj

    if isinstance(obj, UserString):
        # obj.data can only be a regular string. Please see the UserString initializer.
        return obj.data

    return str(obj)


def to_String_for_subst(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj,
    isinstance=isinstance,
    str=str,
    BaseStringTypes=BaseStringTypes,
    SequenceTypes=SequenceTypes,
    UserString=UserString,
) -> str:
    """Return a string version of obj for subst usage."""
    # Note that the test cases are sorted by order of probability.
    if isinstance(obj, BaseStringTypes):
        return obj

    if isinstance(obj, SequenceTypes):
        return ' '.join([to_String_for_subst(e) for e in obj])

    if isinstance(obj, UserString):
        # obj.data can only a regular string. Please see the UserString initializer.
        return obj.data

    return str(obj)


def to_String_for_signature(  # pylint: disable=redefined-outer-name,redefined-builtin
    obj, to_String_for_subst=to_String_for_subst, AttributeError=AttributeError,
) -> str:
    """Return a string version of obj for signature usage.

    Like :func:`to_String_for_subst` but has special handling for
    scons objects that have a :meth:`for_signature` method, and for dicts.
    """
    try:
        f = obj.for_signature
    except AttributeError:
        if isinstance(obj, dict):
            # pprint will output dictionary in key sorted order
            # with py3.5 the order was randomized. Depending on dict order
            # which was undefined until py3.6 (where it's by insertion order)
            # was not wise.
            # TODO: Change code when floor is raised to PY36
            return pprint.pformat(obj, width=1000000)
        return to_String_for_subst(obj)
    return f()


def to_Text(data: bytes) -> str:
    """Return bytes data converted to text.

    Useful for whole-file reads where the data needs some interpretation,
    particularly for Scanners.  Attempts to figure out what the encoding of
    the text is based upon the BOM bytes, and then decodes the contents so
    that it's a valid python string.
    """
    _encoding_map = [
        (codecs.BOM_UTF8, 'utf-8'),
        (codecs.BOM_UTF16_LE, 'utf-16le'),
        (codecs.BOM_UTF16_BE, 'utf-16be'),
        (codecs.BOM_UTF32_LE, 'utf-32le'),
        (codecs.BOM_UTF32_BE, 'utf-32be'),
    ]

    # First look for Byte-order-mark sequences to identify the encoding.
    # Strip these since some codecs do, some don't.
    for bom, encoding in _encoding_map:
        if data.startswith(bom):
            return data[len(bom):].decode(encoding, errors='backslashreplace')

    # If we didn't see a BOM, try UTF-8, then the "preferred" encoding
    # (the files might be written on this system), then finally latin-1.
    # TODO: possibly should be a way for the build to set an encoding.
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
            try:
                import locale
                prefencoding = locale.getpreferredencoding()
                return data.decode(prefencoding)
            except (UnicodeDecodeError, LookupError):
                return data.decode('latin-1', errors='backslashreplace')


def get_env_bool(env, name: str, default: bool=False) -> bool:
    """Convert a construction variable to bool.

    If the value of *name* in dict-like object *env* is 'true', 'yes',
    'y', 'on' (case insensitive) or anything convertible to int that
    yields non-zero, return ``True``; if 'false', 'no', 'n', 'off'
    (case insensitive) or a number that converts to integer zero return
    ``False``.  Otherwise, or if *name* is not found, return the value
    of *default*.

    Args:
        env: construction environment, or any dict-like object.
        name: name of the variable.
        default: value to return if *name* not in *env* or cannot
          be converted (default: False).
    """
    try:
        var = env[name]
    except KeyError:
        return default
    try:
        return bool(int(var))
    except ValueError:
        if str(var).lower() in ('true', 'yes', 'y', 'on'):
            return True

        if str(var).lower() in ('false', 'no', 'n', 'off'):
            return False

        return default


def get_os_env_bool(name: str, default: bool=False) -> bool:
    """Convert an external environment variable to boolean.

    Like :func:`get_env_bool`, but uses :attr:`os.environ` as the lookup dict.
    """
    return get_env_bool(os.environ, name, default)


_get_env_var = re.compile(r'^\$([_a-zA-Z]\w*|{[_a-zA-Z]\w*})$')


def get_environment_var(varstr) -> str | None:
    """Return undecorated construction variable string.

    Determine if *varstr* looks like a reference
    to a single environment variable, like ``"$FOO"`` or ``"${FOO}"``.
    If so, return that variable with no decorations, like ``"FOO"``.
    If not, return ``None``.
    """
    mo = _get_env_var.match(to_String(varstr))
    if mo:
        var = mo.group(1)
        if var[0] == '{':
            return var[1:-1]
        return var

    return None


# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
