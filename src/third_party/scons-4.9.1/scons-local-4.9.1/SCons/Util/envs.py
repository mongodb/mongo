# SPDX-License-Identifier: MIT
#
# Copyright The SCons Foundation

"""
SCons environment utility functions.

Routines for working with environments and construction variables
that don't need the specifics of the Environment class.
"""

from __future__ import annotations

import re
import os
from types import MethodType, FunctionType
from typing import Callable, Any

from .sctypes import is_List, is_Tuple, is_String


def PrependPath(
    oldpath,
    newpath,
    sep=os.pathsep,
    delete_existing: bool = True,
    canonicalize: Callable | None = None,
) -> list | str:
    """Prepend *newpath* path elements to *oldpath*.

    Will only add any particular path once (leaving the first one it
    encounters and ignoring the rest, to preserve path order), and will
    :mod:`os.path.normpath` and :mod:`os.path.normcase` all paths to help
    assure this.  This can also handle the case where *oldpath*
    is a list instead of a string, in which case a list will be returned
    instead of a string. For example:

    >>> p = PrependPath("/foo/bar:/foo", "/biz/boom:/foo")
    >>> print(p)
    /biz/boom:/foo:/foo/bar

    If *delete_existing* is ``False``, then adding a path that exists will
    not move it to the beginning; it will stay where it is in the list.

    >>> p = PrependPath("/foo/bar:/foo", "/biz/boom:/foo", delete_existing=False)
    >>> print(p)
    /biz/boom:/foo/bar:/foo

    If *canonicalize* is not ``None``, it is applied to each element of
    *newpath* before use.
    """
    orig = oldpath
    is_list = True
    paths = orig
    if not is_List(orig) and not is_Tuple(orig):
        paths = paths.split(sep)
        is_list = False

    if is_String(newpath):
        newpaths = newpath.split(sep)
    elif is_List(newpath) or is_Tuple(newpath):
        newpaths = newpath
    else:
        newpaths = [newpath]  # might be a Dir

    if canonicalize:
        newpaths = list(map(canonicalize, newpaths))

    if not delete_existing:
        # First uniquify the old paths, making sure to
        # preserve the first instance (in Unix/Linux,
        # the first one wins), and remembering them in normpaths.
        # Then insert the new paths at the head of the list
        # if they're not already in the normpaths list.
        result = []
        normpaths = []
        for path in paths:
            if not path:
                continue
            normpath = os.path.normpath(os.path.normcase(path))
            if normpath not in normpaths:
                result.append(path)
                normpaths.append(normpath)
        newpaths.reverse()  # since we're inserting at the head
        for path in newpaths:
            if not path:
                continue
            normpath = os.path.normpath(os.path.normcase(path))
            if normpath not in normpaths:
                result.insert(0, path)
                normpaths.append(normpath)
        paths = result

    else:
        newpaths = newpaths + paths  # prepend new paths

        normpaths = []
        paths = []
        # now we add them only if they are unique
        for path in newpaths:
            normpath = os.path.normpath(os.path.normcase(path))
            if path and normpath not in normpaths:
                paths.append(path)
                normpaths.append(normpath)

    if is_list:
        return paths

    return sep.join(paths)


def AppendPath(
    oldpath,
    newpath,
    sep=os.pathsep,
    delete_existing: bool = True,
    canonicalize: Callable | None = None,
) -> list | str:
    """Append *newpath* path elements to *oldpath*.

    Will only add any particular path once (leaving the last one it
    encounters and ignoring the rest, to preserve path order), and will
    :mod:`os.path.normpath` and :mod:`os.path.normcase` all paths to help
    assure this.  This can also handle the case where *oldpath*
    is a list instead of a string, in which case a list will be returned
    instead of a string. For example:

    >>> p = AppendPath("/foo/bar:/foo", "/biz/boom:/foo")
    >>> print(p)
    /foo/bar:/biz/boom:/foo

    If *delete_existing* is ``False``, then adding a path that exists
    will not move it to the end; it will stay where it is in the list.

    >>> p = AppendPath("/foo/bar:/foo", "/biz/boom:/foo", delete_existing=False)
    >>> print(p)
    /foo/bar:/foo:/biz/boom

    If *canonicalize* is not ``None``, it is applied to each element of
    *newpath* before use.
    """
    orig = oldpath
    is_list = True
    paths = orig
    if not is_List(orig) and not is_Tuple(orig):
        paths = paths.split(sep)
        is_list = False

    if is_String(newpath):
        newpaths = newpath.split(sep)
    elif is_List(newpath) or is_Tuple(newpath):
        newpaths = newpath
    else:
        newpaths = [newpath]  # might be a Dir

    if canonicalize:
        newpaths = list(map(canonicalize, newpaths))

    if not delete_existing:
        # add old paths to result, then
        # add new paths if not already present
        # (I thought about using a dict for normpaths for speed,
        # but it's not clear hashing the strings would be faster
        # than linear searching these typically short lists.)
        result = []
        normpaths = []
        for path in paths:
            if not path:
                continue
            result.append(path)
            normpaths.append(os.path.normpath(os.path.normcase(path)))
        for path in newpaths:
            if not path:
                continue
            normpath = os.path.normpath(os.path.normcase(path))
            if normpath not in normpaths:
                result.append(path)
                normpaths.append(normpath)
        paths = result
    else:
        # start w/ new paths, add old ones if not present,
        # then reverse.
        newpaths = paths + newpaths  # append new paths
        newpaths.reverse()

        normpaths = []
        paths = []
        # now we add them only if they are unique
        for path in newpaths:
            normpath = os.path.normpath(os.path.normcase(path))
            if path and normpath not in normpaths:
                paths.append(path)
                normpaths.append(normpath)
        paths.reverse()

    if is_list:
        return paths

    return sep.join(paths)


def AddPathIfNotExists(env_dict, key, path, sep: str = os.pathsep) -> None:
    """Add a path element to a construction variable.

    `key` is looked up in `env_dict`, and `path` is added to it if it
    is not already present. `env_dict[key]` is assumed to be in the
    format of a PATH variable: a list of paths separated by `sep` tokens.

    >>> env = {'PATH': '/bin:/usr/bin:/usr/local/bin'}
    >>> AddPathIfNotExists(env, 'PATH', '/opt/bin')
    >>> print(env['PATH'])
    /opt/bin:/bin:/usr/bin:/usr/local/bin
    """
    try:
        is_list = True
        paths = env_dict[key]
        if not is_List(env_dict[key]):
            paths = paths.split(sep)
            is_list = False
        if os.path.normcase(path) not in list(map(os.path.normcase, paths)):
            paths = [path] + paths
        if is_list:
            env_dict[key] = paths
        else:
            env_dict[key] = sep.join(paths)
    except KeyError:
        env_dict[key] = path


class MethodWrapper:
    """A generic Wrapper class that associates a method with an object.

    As part of creating this MethodWrapper object an attribute with the
    specified name (by default, the name of the supplied method) is added
    to the underlying object.  When that new "method" is called, our
    :meth:`__call__` method adds the object as the first argument, simulating
    the Python behavior of supplying "self" on method calls.

    We hang on to the name by which the method was added to the underlying
    base class so that we can provide a method to "clone" ourselves onto
    a new underlying object being copied (without which we wouldn't need
    to save that info).
    """
    def __init__(self, obj: Any, method: Callable, name: str | None = None) -> None:
        if name is None:
            name = method.__name__
        self.object = obj
        self.method = method
        self.name: str = name
        setattr(self.object, name, self)

    def __call__(self, *args, **kwargs):
        nargs = (self.object,) + args
        return self.method(*nargs, **kwargs)

    def clone(self, new_object):
        """
        Returns an object that re-binds the underlying "method" to
        the specified new object.
        """
        return self.__class__(new_object, self.method, self.name)


# The original idea for AddMethod() came from the
# following post to the ActiveState Python Cookbook:
#
# ASPN: Python Cookbook : Install bound methods in an instance
# https://code.activestate.com/recipes/223613
#
# Changed as follows:
# * Switched the installmethod() "object" and "function" arguments,
#   so the order reflects that the left-hand side is the thing being
#   "assigned to" and the right-hand side is the value being assigned.
# * The instance/class detection is changed a bit, as it's all
#   new-style classes now with Py3.
# * The by-hand construction of the function object from renamefunction()
#   is not needed, the remaining bit is now used inline in AddMethod.


def AddMethod(obj, function: Callable, name: str | None = None) -> None:
    """Add a method to an object.

    Adds *function* to *obj* if *obj* is a class object.
    Adds *function* as a bound method if *obj* is an instance object.
    If *obj* looks like an environment instance, use :class:`~SCons.Util.MethodWrapper`
    to add it.  If *name* is supplied it is used as the name of *function*.

    Although this works for any class object, the intent as a public
    API is to be used on Environment, to be able to add a method to all
    construction environments; it is preferred to use ``env.AddMethod``
    to add to an individual environment.

    >>> class A:
    ...    ...

    >>> a = A()

    >>> def f(self, x, y):
    ...    self.z = x + y

    >>> AddMethod(A, f, "add")
    >>> a.add(2, 4)
    >>> print(a.z)
    6
    >>> a.data = ['a', 'b', 'c', 'd', 'e', 'f']
    >>> AddMethod(a, lambda self, i: self.data[i], "listIndex")
    >>> print(a.listIndex(3))
    d

    """
    if name is None:
        name = function.__name__
    else:
        # "rename"
        function = FunctionType(
            function.__code__, function.__globals__, name, function.__defaults__
        )

    method: MethodType | MethodWrapper | Callable

    if hasattr(obj, '__class__') and obj.__class__ is not type:
        # obj is an instance, so it gets a bound method.
        if hasattr(obj, "added_methods"):
            method = MethodWrapper(obj, function, name)
            obj.added_methods.append(method)
        else:
            method = MethodType(function, obj)
    else:
        # obj is a class
        method = function

    setattr(obj, name, method)


# This routine is used to validate that a construction var name can be used
# as a Python identifier, which we require. However, Python 3 introduced an
# isidentifier() string method so there's really not any need for it now.
_is_valid_var_re = re.compile(r'[_a-zA-Z]\w*$')

def is_valid_construction_var(varstr: str) -> bool:
    """Return True if *varstr* is a legitimate name of a construction variable."""
    return bool(_is_valid_var_re.match(varstr))

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
