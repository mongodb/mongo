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

"""Base class for construction Environments.

These are the primary objects used to communicate dependency and
construction information to the build engine.

Keyword arguments supplied when the construction Environment is created
are construction variables used to initialize the Environment.
"""

from __future__ import annotations

import copy
import os
import sys
import re
import shlex
from collections import UserDict, UserList, deque
from subprocess import PIPE, DEVNULL
from typing import TYPE_CHECKING, Callable, Collection, Sequence

import SCons.Action
import SCons.Builder
import SCons.Debug
from SCons.Debug import logInstanceCreation
import SCons.Defaults
from SCons.Errors import UserError, BuildError
import SCons.Memoize
import SCons.Node
import SCons.Node.Alias
import SCons.Node.FS
import SCons.Node.Python
import SCons.Platform
import SCons.SConf
import SCons.SConsign
import SCons.Subst
import SCons.Tool
import SCons.Warnings
from SCons.Util import (
    AppendPath,
    CLVar,
    LogicalLines,
    MethodWrapper,
    PrependPath,
    Split,
    WhereIs,
    flatten,
    is_Dict,
    is_List,
    is_Scalar,
    is_Sequence,
    is_String,
    is_Tuple,
    semi_deepcopy,
    semi_deepcopy_dict,
    to_String_for_subst,
    uniquer_hashables,
)

if TYPE_CHECKING:
    from SCons.Executor import Executor

class _Null:
    pass

_null = _Null

_warn_copy_deprecated = True
_warn_source_signatures_deprecated = True
_warn_target_signatures_deprecated = True

CleanTargets = {}
CalculatorArgs = {}

def alias_builder(env, target, source) -> None:
    pass

AliasBuilder = SCons.Builder.Builder(
    action=alias_builder,
    target_factory=SCons.Node.Alias.default_ans.Alias,
    source_factory=SCons.Node.FS.Entry,
    multi=True,
    is_explicit=None,
    name='AliasBuilder',
)

def apply_tools(env, tools, toolpath) -> None:
    # Store the toolpath in the Environment.
    # This is expected to work even if no tools are given, so do this first.
    if toolpath is not None:
        env['toolpath'] = toolpath
    if not tools:
        return

    # Filter out null tools from the list.
    for tool in [_f for _f in tools if _f]:
        if is_List(tool) or is_Tuple(tool):
            # toolargs should be a dict of kw args
            toolname, toolargs, *rest = tool
            _ = env.Tool(toolname, **toolargs)
        else:
            _ = env.Tool(tool)

# These names are (or will be) controlled by SCons; users should never
# set or override them.  The warning can optionally be turned off,
# but scons will still ignore the illegal variable names even if it's off.
reserved_construction_var_names = [
    'CHANGED_SOURCES',
    'CHANGED_TARGETS',
    'SOURCE',
    'SOURCES',
    'TARGET',
    'TARGETS',
    'UNCHANGED_SOURCES',
    'UNCHANGED_TARGETS',
]

future_reserved_construction_var_names = [
    #'HOST_OS',
    #'HOST_ARCH',
    #'HOST_CPU',
]

def copy_non_reserved_keywords(dict):
    result = semi_deepcopy(dict)
    for k in result.copy().keys():
        if k in reserved_construction_var_names:
            msg = "Ignoring attempt to set reserved variable `$%s'"
            SCons.Warnings.warn(SCons.Warnings.ReservedVariableWarning, msg % k)
            del result[k]
    return result

def _set_reserved(env, key, value) -> None:
    msg = "Ignoring attempt to set reserved variable `$%s'"
    SCons.Warnings.warn(SCons.Warnings.ReservedVariableWarning, msg % key)

def _set_future_reserved(env, key, value) -> None:
    env._dict[key] = value
    msg = "`$%s' will be reserved in a future release and setting it will become ignored"
    SCons.Warnings.warn(SCons.Warnings.FutureReservedVariableWarning, msg % key)

def _set_BUILDERS(env, key, value):
    try:
        bd = env._dict[key]
        for k in bd.copy().keys():
            del bd[k]
    except KeyError:
        bd = BuilderDict(bd, env)
        env._dict[key] = bd
    for k, v in value.items():
        if not SCons.Builder.is_a_Builder(v):
            raise UserError('%s is not a Builder.' % repr(v))
    bd.update(value)

def _del_SCANNERS(env, key) -> None:
    del env._dict[key]
    env.scanner_map_delete()

def _set_SCANNERS(env, key, value) -> None:
    env._dict[key] = value
    env.scanner_map_delete()

def _delete_duplicates(l, keep_last):
    """Delete duplicates from a sequence, keeping the first or last."""
    seen=set()
    result=[]
    if keep_last:           # reverse in & out, then keep first
        l.reverse()
    for i in l:
        try:
            if i not in seen:
                result.append(i)
                seen.add(i)
        except TypeError:
            # probably unhashable.  Just keep it.
            result.append(i)
    if keep_last:
        result.reverse()
    return result


def _add_cppdefines(
    env_dict: dict,
    val,  # add annotation?
    prepend: bool = False,
    unique: bool = False,
    delete_existing: bool = False,
) -> None:
    """Adds to ``CPPDEFINES``, using the rules for C preprocessor macros.

    This is split out from regular construction variable addition because
    these entries can express either a macro with a replacement value or
    one without.  A macro with replacement value can be supplied as *val*
    in three ways: as a combined string ``"name=value"``; as a tuple
    ``(name, value)``, or as an entry in a dictionary ``{"name": value}``.
    A list argument with multiple macros can also be given.

    Additions can be unconditional (duplicates allowed) or uniquing (no dupes).

    Note if a replacement value is supplied, *unique* requires a full
    match to decide uniqueness - both the macro name and the replacement.
    The inner :func:`_is_in` is used to figure that out.

    Args:
        env_dict: the dictionary containing the ``CPPDEFINES`` to be modified.
        val: the value to add, can be string, sequence or dict
        prepend: whether to put *val* in front or back.
        unique: whether to add *val* if it already exists.
        delete_existing: if *unique* is true, add *val* after removing previous.

    .. versionadded:: 4.5.0
    """

    def _add_define(item, defines: deque, prepend: bool = False) -> None:
        """Convenience function to prepend/append a single value.

        Sole purpose is to shorten code in the outer function.
        """
        if prepend:
            defines.appendleft(item)
        else:
            defines.append(item)


    def _is_in(item, defines: deque):
        """Returns match for *item* if found in *defines*.

        Accounts for type differences: tuple ("FOO", "BAR"), list
        ["FOO", "BAR"], string "FOO=BAR" and dict {"FOO": "BAR"} all
        differ as far as Python equality comparison is concerned, but
        are the same for purposes of creating the preprocessor macro.
        Also an unvalued string should match something like ``("FOO", None)``.
        Since the caller may wish to remove a matched entry, we need to
        return it - cannot remove *item* itself unless it happened to
        be an exact (type) match.

        Called from a place we know *defines* is always a deque, and
        *item* will not be a dict, so don't need to do much type checking.
        If this ends up used more generally, would need to adjust that.

        Note implied assumption that members of a list-valued define
        will not be dicts - we cannot actually guarantee this, since
        if the initial add is a list its contents are not converted.
        """
        def _macro_conv(v) -> list:
            """Normalizes a macro to a list for comparisons."""
            if is_Tuple(v):
                return list(v)
            elif is_String(v):
                rv = v.split("=")
                if len(rv) == 1:
                    return [v, None]
                return rv
            return v

        if item in defines:  # cheap check first
            return item

        item = _macro_conv(item)
        for define in defines:
            if item == _macro_conv(define):
                return define

        return False

    key = 'CPPDEFINES'
    try:
        defines = env_dict[key]
    except KeyError:
        # This is a new entry, just save it as is. Defer conversion to
        # preferred type until someone tries to amend the value.
        # processDefines has no problem with unconverted values if it
        # gets called without any later additions.
        if is_String(val):
            env_dict[key] = val.split()
        else:
            env_dict[key] = val
        return

    # Convert type of existing to deque (if necessary) to simplify processing
    # of additions - inserting at either end is cheap. Deferred conversion
    # is also useful in case CPPDEFINES was set initially without calling
    # through here (e.g. Environment kwarg, or direct assignment).
    if isinstance(defines, deque):
        # Already a deque? do nothing. Explicit check is so we don't get
        # picked up by the is_list case below.
        pass
    elif is_String(defines):
        env_dict[key] = deque(defines.split())
    elif is_Tuple(defines):
        if len(defines) > 2:
            raise SCons.Errors.UserError(
                f"Invalid tuple in CPPDEFINES: {defines!r}, must be a two-tuple"
            )
        env_dict[key] = deque([defines])
    elif is_List(defines):
        # a little extra work in case the initial container has dict
        # item(s) inside it, so those can be matched by _is_in().
        result = deque()
        for define in defines:
            if is_Dict(define):
                result.extend(define.items())
            else:
                result.append(define)
        env_dict[key] = result
    elif is_Dict(defines):
        env_dict[key] = deque(defines.items())
    else:
        env_dict[key] = deque(defines)
    defines = env_dict[key]  # in case we reassigned due to conversion

    # now actually do the addition.
    if is_Dict(val):
        # Unpack the dict while applying to existing
        for item in val.items():
            if unique:
                match = _is_in(item, defines)
                if match and delete_existing:
                    defines.remove(match)
                    _add_define(item, defines, prepend)
                elif not match:
                    _add_define(item, defines, prepend)
            else:
                _add_define(item, defines, prepend)

    elif is_String(val):
        for v in val.split():
            if unique:
                match = _is_in(v, defines)
                if match and delete_existing:
                    defines.remove(match)
                    _add_define(v, defines, prepend)
                elif not match:
                    _add_define(v, defines, prepend)
            else:
                _add_define(v, defines, prepend)

    # A tuple appended to anything should yield -Dkey=value
    elif is_Tuple(val):
        if len(val) > 2:
            raise SCons.Errors.UserError(
                f"Invalid tuple added to CPPDEFINES: {val!r}, "
                "must be a two-tuple"
            )
        if len(val) == 1:
            val = (val[0], None)  # normalize
        if not is_Scalar(val[0]) or not is_Scalar(val[1]):
            raise SCons.Errors.UserError(
                f"Invalid tuple added to CPPDEFINES: {val!r}, "
                "values must be scalar"
            )
        if unique:
            match = _is_in(val, defines)
            if match and delete_existing:
                defines.remove(match)
                _add_define(val, defines, prepend)
            elif not match:
                _add_define(val, defines, prepend)
        else:
            _add_define(val, defines, prepend)

    elif is_List(val):
        tmp = []
        for item in val:
            if unique:
                match = _is_in(item, defines)
                if match and delete_existing:
                    defines.remove(match)
                    tmp.append(item)
                elif not match:
                    tmp.append(item)
            else:
                tmp.append(item)

        if prepend:
            defines.extendleft(tmp)
        else:
            defines.extend(tmp)

    # else:  # are there any other cases? processDefines doesn't think so.


# The following is partly based on code in a comment added by Peter
# Shannon at the following page (there called the "transplant" class):
#
# ASPN : Python Cookbook : Dynamically added methods to a class
# https://code.activestate.com/recipes/81732/
#
# We had independently been using the idiom as BuilderWrapper, but
# factoring out the common parts into this base class, and making
# BuilderWrapper a subclass that overrides __call__() to enforce specific
# Builder calling conventions, simplified some of our higher-layer code.
#
# Note: MethodWrapper moved to SCons.Util as it was needed there
# and otherwise we had a circular import problem.

class BuilderWrapper(MethodWrapper):
    """
    A MethodWrapper subclass that that associates an environment with
    a Builder.

    This mainly exists to wrap the __call__() function so that all calls
    to Builders can have their argument lists massaged in the same way
    (treat a lone argument as the source, treat two arguments as target
    then source, make sure both target and source are lists) without
    having to have cut-and-paste code to do it.

    As a bit of obsessive backwards compatibility, we also intercept
    attempts to get or set the "env" or "builder" attributes, which were
    the names we used before we put the common functionality into the
    MethodWrapper base class.  We'll keep this around for a while in case
    people shipped Tool modules that reached into the wrapper (like the
    Tool/qt.py module does, or did).  There shouldn't be a lot attribute
    fetching or setting on these, so a little extra work shouldn't hurt.
    """
    def __call__(self, target=None, source=_null, *args, **kw):
        if source is _null:
            source = target
            target = None
        if target is not None and not is_List(target):
            target = [target]
        if source is not None and not is_List(source):
            source = [source]
        return super().__call__(target, source, *args, **kw)

    def __repr__(self) -> str:
        return '<BuilderWrapper %s>' % repr(self.name)

    def __str__(self) -> str:
        return self.__repr__()

    def __getattr__(self, name):
        if name == 'env':
            return self.object
        elif name == 'builder':
            return self.method
        else:
            raise AttributeError(name)

    def __setattr__(self, name, value) -> None:
        if name == 'env':
            self.object = value
        elif name == 'builder':
            self.method = value
        else:
            self.__dict__[name] = value

    # This allows a Builder to be executed directly
    # through the Environment to which it's attached.
    # In practice, we shouldn't need this, because
    # builders actually get executed through a Node.
    # But we do have a unit test for this, and can't
    # yet rule out that it would be useful in the
    # future, so leave it for now.
    #def execute(self, **kw):
    #    kw['env'] = self.env
    #    self.builder.execute(**kw)

class BuilderDict(UserDict):
    """This is a dictionary-like class used by an Environment to hold
    the Builders.  We need to do this because every time someone changes
    the Builders in the Environment's BUILDERS dictionary, we must
    update the Environment's attributes."""
    def __init__(self, mapping, env) -> None:
        # Set self.env before calling the superclass initialization,
        # because it will end up calling our other methods, which will
        # need to point the values in this dictionary to self.env.
        self.env = env
        super().__init__(mapping)

    def __semi_deepcopy__(self):
        # These cannot be copied since they would both modify the same builder object, and indeed
        # just copying would modify the original builder
        raise TypeError( 'cannot semi_deepcopy a BuilderDict' )

    def __setitem__(self, item, val) -> None:
        try:
            method = getattr(self.env, item).method
        except AttributeError:
            pass
        else:
            self.env.RemoveMethod(method)
        super().__setitem__(item, val)
        BuilderWrapper(self.env, val, item)

    def __delitem__(self, item) -> None:
        super().__delitem__(item)
        delattr(self.env, item)

    def update(self, mapping) -> None:
        for i, v in mapping.items():
            self.__setitem__(i, v)


class SubstitutionEnvironment:
    """Base class for different flavors of construction environments.

    This class contains a minimal set of methods that handle construction
    variable expansion and conversion of strings to Nodes, which may or
    may not be actually useful as a stand-alone class.  Which methods
    ended up in this class is pretty arbitrary right now.  They're
    basically the ones which we've empirically determined are common to
    the different construction environment subclasses, and most of the
    others that use or touch the underlying dictionary of construction
    variables.

    Eventually, this class should contain all the methods that we
    determine are necessary for a "minimal" interface to the build engine.
    A full "native Python" SCons environment has gotten pretty heavyweight
    with all of the methods and Tools and construction variables we've
    jammed in there, so it would be nice to have a lighter weight
    alternative for interfaces that don't need all of the bells and
    whistles.  (At some point, we'll also probably rename this class
    "Base," since that more reflects what we want this class to become,
    but because we've released comments that tell people to subclass
    Environment.Base to create their own flavors of construction
    environment, we'll save that for a future refactoring when this
    class actually becomes useful.)

    Special note: methods here and in actual child classes might be called
    via proxy from an :class:`OverrideEnvironment`, which isn't in the
    class inheritance chain. Take care that methods called with a *self*
    that's really an ``OverrideEnvironment`` don't make bad assumptions.
    """

    def __init__(self, **kw) -> None:
        """Initialization of an underlying SubstitutionEnvironment class.
        """
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Environment.SubstitutionEnvironment')
        self.fs = SCons.Node.FS.get_default_fs()
        self.ans = SCons.Node.Alias.default_ans
        self.lookup_list = SCons.Node.arg2nodes_lookups
        self._dict = kw.copy()
        self._init_special()
        self.added_methods = []
        #self._memo = {}

    def _init_special(self) -> None:
        """Initial the dispatch tables for special handling of
        special construction variables."""
        self._special_del = {}
        self._special_del['SCANNERS'] = _del_SCANNERS

        self._special_set = {}
        for key in reserved_construction_var_names:
            self._special_set[key] = _set_reserved
        for key in future_reserved_construction_var_names:
            self._special_set[key] = _set_future_reserved
        self._special_set['BUILDERS'] = _set_BUILDERS
        self._special_set['SCANNERS'] = _set_SCANNERS

        # Freeze the keys of self._special_set in a list for use by
        # methods that need to check.
        self._special_set_keys = list(self._special_set.keys())

    def __eq__(self, other):
        """Compare two environments.

        This is used by checks in Builder to determine if duplicate
        targets have environments that would cause the same result.
        The more reliable way (respecting the admonition to avoid poking
        at :attr:`_dict` directly) would be to use ``Dictionary`` so this
        is sure to work even if one or both are are instances of
        :class:`OverrideEnvironment`. However an actual
        ``SubstitutionEnvironment`` doesn't have a ``Dictionary`` method
        That causes problems for unit tests written to excercise
        ``SubsitutionEnvironment`` directly, although nobody else seems
        to ever instantiate one. We count on :class:`OverrideEnvironment`
        to fake the :attr:`_dict` to make things work.
        """
        return self._dict == other._dict

    def __delitem__(self, key) -> None:
        special = self._special_del.get(key)
        if special:
            special(self, key)
        else:
            del self._dict[key]

    def __getitem__(self, key):
        return self._dict[key]

    def __setitem__(self, key, value):
        if key in self._special_set_keys:
            self._special_set[key](self, key, value)
        else:
            # Performance: since this is heavily used, try to avoid checking
            # if the variable is valid unless necessary. bench/__setitem__.py
            # times a bunch of different approaches. Based the most recent
            # run, against Python 3.6-3.13(beta), the best we can do across
            # different combinations of actions is to use a membership test
            # to see if we already have the variable, if so it must already
            # have been checked, so skip; if we do check, "isidentifier()"
            # (new in Python 3 so wasn't in benchmark until recently)
            # on the key is the best.
            if key not in self._dict and not key.isidentifier():
                raise UserError(f"Illegal construction variable {key!r}")
            self._dict[key] = value

    def get(self, key, default=None):
        """Emulates the get() method of dictionaries."""
        return self._dict.get(key, default)

    def __contains__(self, key) -> bool:
        return key in self._dict

    def keys(self):
        """Emulates the keys() method of dictionaries."""
        return self._dict.keys()

    def values(self):
        """Emulates the values() method of dictionaries."""
        return self._dict.values()

    def items(self):
        """Emulates the items() method of dictionaries."""
        return self._dict.items()

    def setdefault(self, key, default=None):
        """Emulates the setdefault() method of dictionaries."""
        return self._dict.setdefault(key, default)

    def arg2nodes(self, args, node_factory=_null, lookup_list=_null, **kw):
        """Converts *args* to a list of nodes.

        Arguments:
           args - filename strings or nodes to convert; nodes are just
              added to the list without further processing.
           node_factory - optional factory to create the nodes; if not
              specified, will use this environment's ``fs.File method.
           lookup_list - optional list of lookup functions to call to
              attempt to find the file referenced by each *args*.
           kw - keyword arguments that represent additional nodes to add.
        """
        if node_factory is _null:
            node_factory = self.fs.File
        if lookup_list is _null:
            lookup_list = self.lookup_list

        if not args:
            return []

        args = flatten(args)

        nodes = []
        for v in args:
            if is_String(v):
                n = None
                for l in lookup_list:
                    n = l(v)
                    if n is not None:
                        break
                if n is not None:
                    if is_String(n):
                        # n = self.subst(n, raw=1, **kw)
                        kw['raw'] = 1
                        n = self.subst(n, **kw)
                        if node_factory:
                            n = node_factory(n)
                    if is_List(n):
                        nodes.extend(n)
                    else:
                        nodes.append(n)
                elif node_factory:
                    # v = node_factory(self.subst(v, raw=1, **kw))
                    kw['raw'] = 1
                    v = node_factory(self.subst(v, **kw))
                    if is_List(v):
                        nodes.extend(v)
                    else:
                        nodes.append(v)
            else:
                nodes.append(v)

        return nodes

    def gvars(self):
        return self._dict

    def lvars(self):
        return {}

    def subst(self, string, raw: int=0, target=None, source=None, conv=None, executor: Executor | None = None, overrides: dict | None = None):
        """Recursively interpolates construction variables from the
        Environment into the specified string, returning the expanded
        result.  Construction variables are specified by a $ prefix
        in the string and begin with an initial underscore or
        alphabetic character followed by any number of underscores
        or alphanumeric characters.  The construction variable names
        may be surrounded by curly braces to separate the name from
        trailing characters.
        """
        gvars = self.gvars()
        lvars = self.lvars()
        lvars['__env__'] = self
        if executor:
            lvars.update(executor.get_lvars())
        return SCons.Subst.scons_subst(string, self, raw, target, source, gvars, lvars, conv, overrides=overrides)

    def subst_kw(self, kw, raw: int=0, target=None, source=None):
        nkw = {}
        for k, v in kw.items():
            k = self.subst(k, raw, target, source)
            if is_String(v):
                v = self.subst(v, raw, target, source)
            nkw[k] = v
        return nkw

    def subst_list(self, string, raw: int=0, target=None, source=None, conv=None, executor: Executor | None = None, overrides: dict | None = None):
        """Calls through to SCons.Subst.scons_subst_list().

        See the documentation for that function.
        """
        gvars = self.gvars()
        lvars = self.lvars()
        lvars['__env__'] = self
        if executor:
            lvars.update(executor.get_lvars())
        return SCons.Subst.scons_subst_list(string, self, raw, target, source, gvars, lvars, conv, overrides=overrides)

    def subst_path(self, path, target=None, source=None):
        """Substitute a path list.

        Turns EntryProxies into Nodes, leaving Nodes (and other objects) as-is.
        """
        if not is_List(path):
            path = [path]

        def s(obj):
            """This is the "string conversion" routine that we have our
            substitutions use to return Nodes, not strings.  This relies
            on the fact that an EntryProxy object has a get() method that
            returns the underlying Node that it wraps, which is a bit of
            architectural dependence that we might need to break or modify
            in the future in response to additional requirements."""
            try:
                get = obj.get
            except AttributeError:
                obj = to_String_for_subst(obj)
            else:
                obj = get()
            return obj

        r = []
        for p in path:
            if is_String(p):
                p = self.subst(p, target=target, source=source, conv=s)
                if is_List(p):
                    if len(p) == 1:
                        p = p[0]
                    else:
                        # We have an object plus a string, or multiple
                        # objects that we need to smush together.  No choice
                        # but to make them into a string.
                        p = ''.join(map(to_String_for_subst, p))
            else:
                p = s(p)
            r.append(p)
        return r

    subst_target_source = subst


    def backtick(self, command) -> str:
        """Emulate command substitution.

        Provides behavior conceptually like POSIX Shell notation
        for running a command in backquotes (backticks) by running
        ``command`` and returning the resulting output string.

        This is not really a public API any longer, it is provided for the
        use of :meth:`ParseFlags` (which supports it using a syntax of
        !command) and :meth:`ParseConfig`.

        Raises:
            OSError: if the external command returned non-zero exit status.
        """
        # common arguments
        kw = {
            "stdin": DEVNULL,
            "stdout": PIPE,
            "stderr": PIPE,
            "universal_newlines": True,
        }
        # if the command is a list, assume it's been quoted
        # othewise force a shell
        if not is_List(command):
            kw["shell"] = True
        # run constructed command
        cp = SCons.Action.scons_subproc_run(self, command, **kw)
        if cp.stderr:
            sys.stderr.write(cp.stderr)
        if cp.returncode:
            raise OSError(f'{command!r} exited {cp.returncode}')
        return cp.stdout


    def AddMethod(self, function, name=None) -> None:
        """
        Adds the specified function as a method of this construction
        environment with the specified name.  If the name is omitted,
        the default name is the name of the function itself.
        """
        method = MethodWrapper(self, function, name)
        self.added_methods.append(method)

    def RemoveMethod(self, function) -> None:
        """
        Removes the specified function's MethodWrapper from the
        added_methods list, so we don't re-bind it when making a clone.
        """
        self.added_methods = [dm for dm in self.added_methods if dm.method is not function]

    def Override(self, overrides):
        """Create an override environment from the current environment.

        Produces a modified environment where the current variables are
        overridden by any same-named variables from the *overrides* dict.

        An override is much more efficient than doing :meth:`~Base.Clone`
        or creating a new Environment because it doesn't copy the construction
        environment dictionary, it just wraps the underlying construction
        environment, and doesn't even create a wrapper object if there
        are no overrides.

        Using this method is preferred over directly instantiating an
        :class:`OverrideEnvirionment` because extra checks are performed,
        substitution takes place, and there is special handling for a
        *parse_flags* keyword argument.

        This method is not currently exposed as part of the public API,
        but is invoked internally when things like builder calls have
        keyword arguments, which are then passed as *overrides* here.
        Some tools also call this explicitly.

        Returns:
           A proxy environment of type :class:`OverrideEnvironment`.
           or the current environment if *overrides* is empty.
        """
        if not overrides: return self
        o = copy_non_reserved_keywords(overrides)
        if not o: return self
        overrides = {}
        merges = None
        for key, value in o.items():
            if key == 'parse_flags':
                merges = value
            else:
                overrides[key] = SCons.Subst.scons_subst_once(value, self, key)
        env = OverrideEnvironment(self, overrides)
        if merges:
            env.MergeFlags(merges)
        return env

    def ParseFlags(self, *flags) -> dict:
        """Return a dict of parsed flags.

        Parse ``flags`` and return a dict with the flags distributed into
        the appropriate construction variable names.  The flags are treated
        as a typical set of command-line flags for a GNU-style toolchain,
        such as might have been generated by one of the {foo}-config scripts,
        and used to populate the entries based on knowledge embedded in
        this method - the choices are not expected to be portable to other
        toolchains.

        If one of the ``flags`` strings begins with a bang (exclamation mark),
        it is assumed to be a command and the rest of the string is executed;
        the result of that evaluation is then added to the dict.
        """
        mapping = {
            'ASFLAGS'       : CLVar(''),
            'CFLAGS'        : CLVar(''),
            'CCFLAGS'       : CLVar(''),
            'CXXFLAGS'      : CLVar(''),
            'CPPDEFINES'    : [],
            'CPPFLAGS'      : CLVar(''),
            'CPPPATH'       : [],
            'FRAMEWORKPATH' : CLVar(''),
            'FRAMEWORKS'    : CLVar(''),
            'LIBPATH'       : [],
            'LIBS'          : [],
            'LINKFLAGS'     : CLVar(''),
            'RPATH'         : [],
        }

        def do_parse(arg: str | Sequence) -> None:
            if not arg:
                return

            # if arg is a sequence, recurse with each element
            if not is_String(arg):
                for t in arg: do_parse(t)
                return

            # if arg is a command, execute it
            if arg[0] == '!':
                arg = self.backtick(arg[1:])

            # utility function to deal with -D option
            def append_define(name, mapping=mapping) -> None:
                t = name.split('=')
                if len(t) == 1:
                    mapping['CPPDEFINES'].append(name)
                else:
                    mapping['CPPDEFINES'].append([t[0], '='.join(t[1:])])

            # Loop through the flags and add them to the appropriate variable.
            # This tries to strike a balance between checking for all possible
            # flags and keeping the logic to a finite size, so it doesn't
            # check for some that don't occur often.  It particular, if the
            # flag is not known to occur in a config script and there's a way
            # of passing the flag to the right place (by wrapping it in a -W
            # flag, for example) we don't check for it.  Note that most
            # preprocessor options are not handled, since unhandled options
            # are placed in CCFLAGS, so unless the preprocessor is invoked
            # separately, these flags will still get to the preprocessor.
            # Other options not currently handled:
            #  -iqoutedir      (preprocessor search path)
            #  -u symbol       (linker undefined symbol)
            #  -s              (linker strip files)
            #  -static*        (linker static binding)
            #  -shared*        (linker dynamic binding)
            #  -symbolic       (linker global binding)
            #  -R dir          (deprecated linker rpath)
            # IBM compilers may also accept -qframeworkdir=foo

            params = shlex.split(arg)
            append_next_arg_to = None   # for multi-word args
            for arg in params:
                if append_next_arg_to:
                    # these are the second pass for options where the
                    # option-argument follows as a second word.
                    if append_next_arg_to == 'CPPDEFINES':
                        append_define(arg)
                    elif append_next_arg_to == '-include':
                        t = ('-include', self.fs.File(arg))
                        mapping['CCFLAGS'].append(t)
                    elif append_next_arg_to == '-imacros':
                        t = ('-imacros', self.fs.File(arg))
                        mapping['CCFLAGS'].append(t)
                    elif append_next_arg_to == '-isysroot':
                        t = ('-isysroot', arg)
                        mapping['CCFLAGS'].append(t)
                        mapping['LINKFLAGS'].append(t)
                    elif append_next_arg_to == '-isystem':
                        t = ('-isystem', arg)
                        mapping['CCFLAGS'].append(t)
                    elif append_next_arg_to == '-iquote':
                        t = ('-iquote', arg)
                        mapping['CCFLAGS'].append(t)
                    elif append_next_arg_to == '-idirafter':
                        t = ('-idirafter', arg)
                        mapping['CCFLAGS'].append(t)
                    elif append_next_arg_to == '-arch':
                        t = ('-arch', arg)
                        mapping['CCFLAGS'].append(t)
                        mapping['LINKFLAGS'].append(t)
                    elif append_next_arg_to == '--param':
                        t = ('--param', arg)
                        mapping['CCFLAGS'].append(t)
                    else:
                        mapping[append_next_arg_to].append(arg)
                    append_next_arg_to = None
                elif arg[0] not in ['-', '+']:
                    mapping['LIBS'].append(self.fs.File(arg))
                elif arg == '-dylib_file':
                    mapping['LINKFLAGS'].append(arg)
                    append_next_arg_to = 'LINKFLAGS'
                elif arg[:2] == '-L':
                    if arg[2:]:
                        mapping['LIBPATH'].append(arg[2:])
                    else:
                        append_next_arg_to = 'LIBPATH'
                elif arg[:2] == '-l':
                    if arg[2:]:
                        mapping['LIBS'].append(arg[2:])
                    else:
                        append_next_arg_to = 'LIBS'
                elif arg[:2] == '-I':
                    if arg[2:]:
                        mapping['CPPPATH'].append(arg[2:])
                    else:
                        append_next_arg_to = 'CPPPATH'
                elif arg[:4] == '-Wa,':
                    mapping['ASFLAGS'].append(arg[4:])
                    mapping['CCFLAGS'].append(arg)
                elif arg[:4] == '-Wl,':
                    if arg[:11] == '-Wl,-rpath=':
                        mapping['RPATH'].append(arg[11:])
                    elif arg[:7] == '-Wl,-R,':
                        mapping['RPATH'].append(arg[7:])
                    elif arg[:6] == '-Wl,-R':
                        mapping['RPATH'].append(arg[6:])
                    else:
                        mapping['LINKFLAGS'].append(arg)
                elif arg[:4] == '-Wp,':
                    mapping['CPPFLAGS'].append(arg)
                elif arg[:2] == '-D':
                    if arg[2:]:
                        append_define(arg[2:])
                    else:
                        append_next_arg_to = 'CPPDEFINES'
                elif arg == '-framework':
                    append_next_arg_to = 'FRAMEWORKS'
                elif arg[:14] == '-frameworkdir=':
                    mapping['FRAMEWORKPATH'].append(arg[14:])
                elif arg[:2] == '-F':
                    if arg[2:]:
                        mapping['FRAMEWORKPATH'].append(arg[2:])
                    else:
                        append_next_arg_to = 'FRAMEWORKPATH'
                elif arg in (
                    '-mno-cygwin',
                    '-pthread',
                    '-openmp',
                    '-fmerge-all-constants',
                    '-fopenmp',
                ) or arg.startswith('-fsanitize'):
                    mapping['CCFLAGS'].append(arg)
                    mapping['LINKFLAGS'].append(arg)
                elif arg == '-mwindows':
                    mapping['LINKFLAGS'].append(arg)
                elif arg[:5] == '-std=':
                    if '++' in arg[5:]:
                        key = 'CXXFLAGS'
                    else:
                        key = 'CFLAGS'
                    mapping[key].append(arg)
                elif arg.startswith('-stdlib='):
                    mapping['CXXFLAGS'].append(arg)
                elif arg[0] == '+':
                    mapping['CCFLAGS'].append(arg)
                    mapping['LINKFLAGS'].append(arg)
                elif arg in [
                    '-include',
                    '-imacros',
                    '-isysroot',
                    '-isystem',
                    '-iquote',
                    '-idirafter',
                    '-arch',
                    '--param',
                ]:
                    append_next_arg_to = arg
                else:
                    mapping['CCFLAGS'].append(arg)

        for arg in flags:
            do_parse(arg)
        return mapping

    def MergeFlags(self, args, unique: bool=True) -> None:
        """Merge flags into construction variables.

        Merges the flags from *args* into this construction environent.
        If *args* is not a dict, it is first converted to one with
        flags distributed into appropriate construction variables.
        See :meth:`ParseFlags`.

        As a side effect, if *unique* is true, a new object is created
        for each modified construction variable by the loop at the end.
        This is silently expected by the :meth:`Override` *parse_flags*
        functionality, which does not want to share the list (or whatever)
        with the environment being overridden.

        Args:
            args: flags to merge
            unique: merge flags rather than appending (default: True).
                When merging, path variables are retained from the front,
                other construction variables from the end.
        """
        if not is_Dict(args):
            args = self.ParseFlags(args)

        if not unique:
            self.Append(**args)
            return

        for key, value in args.items():
            if not value:
                continue
            value = Split(value)
            try:
                orig = self[key]
            except KeyError:
                orig = value
            else:
                if not orig:
                    orig = value
                elif value:
                    # Add orig and value.  The logic here was lifted from
                    # part of env.Append() (see there for a lot of comments
                    # about the order in which things are tried) and is
                    # used mainly to handle coercion of strings to CLVar to
                    # "do the right thing" given (e.g.) an original CCFLAGS
                    # string variable like '-pipe -Wall'.
                    try:
                        orig = orig + value
                    except (KeyError, TypeError):
                        # If CPPDEFINES is a deque, adding value (a list)
                        # results in TypeError, so we handle that case here.
                        # Just in case we got called from Override, make
                        # sure we make a copy, because we don't go through
                        # the cleanup loops at the end of the outer for loop,
                        # which implicitly gives us a new object.
                        if isinstance(orig, deque):
                            self[key] = self[key].copy()
                            self.AppendUnique(CPPDEFINES=value, delete_existing=True)
                            continue
                        try:
                            add_to_orig = orig.append
                        except AttributeError:
                            value.insert(0, orig)
                            orig = value
                        else:
                            add_to_orig(value)
            t = []
            if key[-4:] == 'PATH':
                ### keep left-most occurence
                for v in orig:
                    if v not in t:
                        t.append(v)
            else:
                ### keep right-most occurence
                for v in orig[::-1]:
                    if v not in t:
                        t.insert(0, v)

            self[key] = t


def default_decide_source(dependency, target, prev_ni, repo_node=None):
    f = SCons.Defaults.DefaultEnvironment().decide_source
    return f(dependency, target, prev_ni, repo_node)


def default_decide_target(dependency, target, prev_ni, repo_node=None):
    f = SCons.Defaults.DefaultEnvironment().decide_target
    return f(dependency, target, prev_ni, repo_node)


def default_copy_from_cache(env, src, dst):
    return SCons.CacheDir.CacheDir.copy_from_cache(env, src, dst)


def default_copy_to_cache(env, src, dst):
    return SCons.CacheDir.CacheDir.copy_to_cache(env, src, dst)


class Base(SubstitutionEnvironment):
    """Base class for "real" construction Environments.

    These are the primary objects used to communicate dependency
    and construction information to the build engine.

    Keyword arguments supplied when the construction Environment
    is created are construction variables used to initialize the
    Environment.
    """

    #######################################################################
    # This is THE class for interacting with the SCons build engine,
    # and it contains a lot of stuff, so we're going to try to keep this
    # a little organized by grouping the methods.
    #######################################################################

    #######################################################################
    # Methods that make an Environment act like a dictionary.  These have
    # the expected standard names for Python mapping objects.  Note that
    # we don't actually make an Environment a subclass of UserDict for
    # performance reasons.  Note also that we only supply methods for
    # dictionary functionality that we actually need and use.
    #######################################################################

    def __init__(
        self,
        platform=None,
        tools=None,
        toolpath=None,
        variables=None,
        parse_flags=None,
        **kw
    ) -> None:
        """Initialization of a basic SCons construction environment.

        Sets up special construction variables like BUILDER,
        PLATFORM, etc., and searches for and applies available Tools.

        Note that we do *not* call the underlying base class
        (SubsitutionEnvironment) initialization, because we need to
        initialize things in a very specific order that doesn't work
        with the much simpler base class initialization.
        """
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Environment.Base')
        self._memo = {}
        self.fs = SCons.Node.FS.get_default_fs()
        self.ans = SCons.Node.Alias.default_ans
        self.lookup_list = SCons.Node.arg2nodes_lookups
        self._dict = semi_deepcopy(SCons.Defaults.ConstructionEnvironment)
        self._init_special()
        self.added_methods = []

        # We don't use AddMethod, or define these as methods in this
        # class, because we *don't* want these functions to be bound
        # methods.  They need to operate independently so that the
        # settings will work properly regardless of whether a given
        # target ends up being built with a Base environment or an
        # OverrideEnvironment or what have you.
        self.decide_target = default_decide_target
        self.decide_source = default_decide_source

        self.cache_timestamp_newer = False

        self._dict['BUILDERS'] = BuilderDict(self._dict['BUILDERS'], self)

        if platform is None:
            platform = self._dict.get('PLATFORM', None)
            if platform is None:
                platform = SCons.Platform.Platform()
        if is_String(platform):
            platform = SCons.Platform.Platform(platform)
        self._dict['PLATFORM'] = str(platform)
        platform(self)

        # these should be set by the platform, backstop just in case
        self._dict['HOST_OS'] = self._dict.get('HOST_OS', None)
        self._dict['HOST_ARCH'] = self._dict.get('HOST_ARCH', None)

        # these are not currently set by the platform, give them a default
        self._dict['TARGET_OS'] = self._dict.get('TARGET_OS', None)
        self._dict['TARGET_ARCH'] = self._dict.get('TARGET_ARCH', None)

        # Apply the passed-in and customizable variables to the
        # environment before calling the tools, because they may use
        # some of them during initialization.
        if 'options' in kw:
            # Backwards compatibility:  they may stll be using the
            # old "options" keyword.
            variables = kw['options']
            del kw['options']
        self.Replace(**kw)
        keys = list(kw.keys())
        if variables:
            keys = keys + list(variables.keys())
            variables.Update(self)

        save = {}
        for k in keys:
            try:
                save[k] = self._dict[k]
            except KeyError:
                # No value may have been set if they tried to pass in a
                # reserved variable name like TARGETS.
                pass

        SCons.Tool.Initializers(self)

        if tools is None:
            tools = self._dict.get('TOOLS', ['default'])
        else:
            # for a new env, if we didn't use TOOLS, make sure it starts empty
            # so it only shows tools actually initialized.
            self._dict['TOOLS'] = []
        apply_tools(self, tools, toolpath)

        # Now restore the passed-in and customized variables
        # to the environment, since the values the user set explicitly
        # should override any values set by the tools.
        for key, val in save.items():
            self._dict[key] = val

        # Finally, apply any flags to be merged in
        if parse_flags:
            self.MergeFlags(parse_flags)

    #######################################################################
    # Utility methods that are primarily for internal use by SCons.
    # These begin with lower-case letters.
    #######################################################################

    def get_builder(self, name):
        """Fetch the builder with the specified name from the environment.
        """
        try:
            return self._dict['BUILDERS'][name]
        except KeyError:
            return None

    def validate_CacheDir_class(self, custom_class=None):
        """Validate the passed custom CacheDir class, or if no args are passed,
        validate the custom CacheDir class from the environment.
        """

        if custom_class is None:
            custom_class = self.get("CACHEDIR_CLASS", SCons.CacheDir.CacheDir)
        if not issubclass(custom_class, SCons.CacheDir.CacheDir):
            raise UserError("Custom CACHEDIR_CLASS %s not derived from CacheDir" % str(custom_class))
        return custom_class

    def get_CacheDir(self):
        try:
            path = self._CacheDir_path
        except AttributeError:
            path = SCons.Defaults.DefaultEnvironment()._CacheDir_path

        cachedir_class = self.validate_CacheDir_class()
        try:
            if (path == self._last_CacheDir_path
                    # this checks if the cachedir class type has changed from what the
                    # instantiated cache dir type is. If the are exactly the same we
                    # can just keep using the existing one, otherwise the user is requesting
                    # something new, so we will re-instantiate below.
                    and type(self._last_CacheDir) is cachedir_class):
                return self._last_CacheDir
        except AttributeError:
            pass

        cd = cachedir_class(path)
        self._last_CacheDir_path = path
        self._last_CacheDir = cd
        return cd

    def get_factory(self, factory, default: str='File'):
        """Return a factory function for creating Nodes for this
        construction environment.
        """
        name = default
        try:
            is_node = issubclass(factory, SCons.Node.FS.Base)
        except TypeError:
            # The specified factory isn't a Node itself--it's
            # most likely None, or possibly a callable.
            pass
        else:
            if is_node:
                # The specified factory is a Node (sub)class.  Try to
                # return the FS method that corresponds to the Node's
                # name--that is, we return self.fs.Dir if they want a Dir,
                # self.fs.File for a File, etc.
                try: name = factory.__name__
                except AttributeError: pass
                else: factory = None
        if not factory:
            # They passed us None, or we picked up a name from a specified
            # class, so return the FS method.  (Note that we *don't*
            # use our own self.{Dir,File} methods because that would
            # cause env.subst() to be called twice on the file name,
            # interfering with files that have $$ in them.)
            factory = getattr(self.fs, name)
        return factory

    @SCons.Memoize.CountMethodCall
    def _gsm(self):
        try:
            return self._memo['_gsm']
        except KeyError:
            pass

        result = {}

        try:
            scanners = self._dict['SCANNERS']
        except KeyError:
            pass
        else:
            # Reverse the scanner list so that, if multiple scanners
            # claim they can scan the same suffix, earlier scanners
            # in the list will overwrite later scanners, so that
            # the result looks like a "first match" to the user.
            if not is_List(scanners):
                scanners = [scanners]
            else:
                scanners = scanners[:] # copy so reverse() doesn't mod original
            scanners.reverse()
            for scanner in scanners:
                for k in scanner.get_skeys(self):
                    if k and self['PLATFORM'] == 'win32':
                        k = k.lower()
                    result[k] = scanner

        self._memo['_gsm'] = result

        return result

    def get_scanner(self, skey):
        """Find the appropriate scanner given a key (usually a file suffix).
        """
        if skey and self['PLATFORM'] == 'win32':
            skey = skey.lower()
        return self._gsm().get(skey)

    def scanner_map_delete(self, kw=None) -> None:
        """Delete the cached scanner map (if we need to).
        """
        try:
            del self._memo['_gsm']
        except KeyError:
            pass

    def _update(self, other) -> None:
        """Private method to update an environment's consvar dict directly.

        Bypasses the normal checks that occur when users try to set items.
        """
        self._dict.update(other)

    def _update_onlynew(self, other) -> None:
        """Private method to add new items to an environment's consvar dict.

        Only adds items from `other` whose keys do not already appear in
        the existing dict; values from `other` are not used for replacement.
        Bypasses the normal checks that occur when users try to set items.
        """
        for k, v in other.items():
            if k not in self._dict:
                self._dict[k] = v

    #######################################################################
    # Public methods for manipulating an Environment.  These begin with
    # upper-case letters.  The essential characteristic of methods in
    # this section is that they do *not* have corresponding same-named
    # global functions.  For example, a stand-alone Append() function
    # makes no sense, because Append() is all about appending values to
    # an Environment's construction variables.
    #######################################################################

    def Append(self, **kw) -> None:
        """Append values to construction variables in an Environment.

        The variable is created if it is not already present.
        """
        kw = copy_non_reserved_keywords(kw)
        for key, val in kw.items():
            if key == 'CPPDEFINES':
                _add_cppdefines(self._dict, val)
                continue

            try:
                orig = self._dict[key]
            except KeyError:
                # No existing var in the environment, so set to the new value.
                self._dict[key] = val
                continue

            try:
                # Check if the original looks like a dict: has .update?
                update_dict = orig.update
            except AttributeError:
                try:
                    # Just try to add them together.  This will work
                    # in most cases, when the original and new values
                    # are compatible types.
                    self._dict[key] = orig + val
                except (KeyError, TypeError):
                    try:
                        # Check if the original is a list: has .append?
                        add_to_orig = orig.append
                    except AttributeError:
                        # The original isn't a list, but the new
                        # value is (by process of elimination),
                        # so insert the original in the new value
                        # (if there's one to insert) and replace
                        # the variable with it.
                        if orig:
                            val.insert(0, orig)
                        self._dict[key] = val
                    else:
                        # The original is a list, so append the new
                        # value to it (if there's a value to append).
                        if val:
                            add_to_orig(val)
                continue

            # The original looks like a dictionary, so update it
            # based on what we think the value looks like.
            # We can't just try adding the value because
            # dictionaries don't have __add__() methods, and
            # things like UserList will incorrectly coerce the
            # original dict to a list (which we don't want).
            if is_List(val):
                for v in val:
                    orig[v] = None
            else:
                try:
                    update_dict(val)
                except (AttributeError, TypeError, ValueError):
                    if is_Dict(val):
                        for k, v in val.items():
                            orig[k] = v
                    else:
                        orig[val] = None

        self.scanner_map_delete(kw)

    def _canonicalize(self, path):
        """Allow Dirs and strings beginning with # for top-relative.

        Note this uses the current env's fs (in self).
        """
        if not is_String(path):  # typically a Dir
            path = str(path)
        if path and path[0] == '#':
            path = str(self.fs.Dir(path))
        return path

    def AppendENVPath(self, name, newpath, envname: str='ENV',
                      sep=os.pathsep, delete_existing: bool=False) -> None:
        """Append path elements to the path *name* in the *envname*
        dictionary for this environment.  Will only add any particular
        path once, and will normpath and normcase all paths to help
        assure this.  This can also handle the case where the env
        variable is a list instead of a string.

        If *delete_existing* is False, a *newpath* element already in the path
        will not be moved to the end (it will be left where it is).
        """

        orig = ''
        if envname in self._dict and name in self._dict[envname]:
            orig = self._dict[envname][name]

        nv = AppendPath(orig, newpath, sep, delete_existing, canonicalize=self._canonicalize)

        if envname not in self._dict:
            self._dict[envname] = {}

        self._dict[envname][name] = nv

    def AppendUnique(self, delete_existing: bool = False, **kw) -> None:
        """Append values uniquely to existing construction variables.

        Similar to :meth:`Append`, but the result may not contain duplicates
        of any values passed for each given key (construction variable),
        so an existing list may need to be pruned first, however it may still
        contain other duplicates.

        If *delete_existing* is true, removes existing values first, so values
        move to the end; otherwise (the default) values are skipped if
        already present.
        """
        kw = copy_non_reserved_keywords(kw)
        for key, val in kw.items():
            if key == 'CPPDEFINES':
                _add_cppdefines(self._dict, val, unique=True, delete_existing=delete_existing)
                continue
            if is_List(val):
                val = _delete_duplicates(val, delete_existing)
            if key not in self._dict or self._dict[key] in ('', None):
                self._dict[key] = val
            elif is_Dict(self._dict[key]) and is_Dict(val):
                self._dict[key].update(val)
            elif is_List(val):
                dk = self._dict[key]
                if not is_List(dk):
                    dk = [dk]
                if delete_existing:
                    dk = [x for x in dk if x not in val]
                else:
                    val = [x for x in val if x not in dk]
                self._dict[key] = dk + val
            else:
                # val is not a list, so presumably a scalar (likely str).
                dk = self._dict[key]
                if is_List(dk):
                    if delete_existing:
                        dk = [x for x in dk if x != val]
                        self._dict[key] = dk + [val]
                    else:
                        if val not in dk:
                            self._dict[key] = dk + [val]
                else:
                    if delete_existing:
                        dk = [x for x in dk if x not in val]
                    self._dict[key] = dk + val
        self.scanner_map_delete(kw)

    def Clone(self, tools=[], toolpath=None, variables=None, parse_flags=None, **kw):
        """Return a copy of a construction Environment.

        The copy is like a Python "deep copy": independent copies are made
        recursively of each object, except that a reference is copied when
        an object is not deep-copyable (like a function).  There are no
        references to any mutable objects in the original environment.

        Unrecognized keyword arguments are taken as construction variable
        assignments.

        Arguments:
           tools: list of tools to initialize.
           toolpath: list of paths to search for tools.
           variables: a :class:`~SCons.Variables.Variables` object to
              use to populate construction variables from command-line
              variables.
           parse_flags: option strings to parse into construction variables.

        .. versionadded:: 4.8.0
              The optional *variables* parameter was added.
        """
        builders = self._dict.get('BUILDERS', {})

        clone = copy.copy(self)
        # BUILDERS is not safe to do a simple copy
        clone._dict = semi_deepcopy_dict(self._dict, ['BUILDERS'])
        clone._dict['BUILDERS'] = BuilderDict(builders, clone)

        # Check the methods added via AddMethod() and re-bind them to
        # the cloned environment.  Only do this if the attribute hasn't
        # been overwritten by the user explicitly and still points to
        # the added method.
        clone.added_methods = []
        for mw in self.added_methods:
            if mw == getattr(self, mw.name):
                clone.added_methods.append(mw.clone(clone))

        clone._memo = {}

        # Apply passed-in variables before the tools
        # so the tools can use the new variables
        kw = copy_non_reserved_keywords(kw)
        new = {}
        for key, value in kw.items():
            new[key] = SCons.Subst.scons_subst_once(value, self, key)
        clone.Replace(**new)
        if variables:
            variables.Update(clone)

        apply_tools(clone, tools, toolpath)

        # apply them again in case the tools overwrote them
        clone.Replace(**new)

        # Finally, apply any flags to be merged in
        if parse_flags:
            clone.MergeFlags(parse_flags)

        if SCons.Debug.track_instances: logInstanceCreation(self, 'Environment.EnvironmentClone')
        return clone

    def _changed_build(self, dependency, target, prev_ni, repo_node=None) -> bool:
        if dependency.changed_state(target, prev_ni, repo_node):
            return True
        return self.decide_source(dependency, target, prev_ni, repo_node)

    def _changed_content(self, dependency, target, prev_ni, repo_node=None) -> bool:
        return dependency.changed_content(target, prev_ni, repo_node)

    def _changed_timestamp_then_content(self, dependency, target, prev_ni, repo_node=None) -> bool:
        return dependency.changed_timestamp_then_content(target, prev_ni, repo_node)

    def _changed_timestamp_newer(self, dependency, target, prev_ni, repo_node=None) -> bool:
        return dependency.changed_timestamp_newer(target, prev_ni, repo_node)

    def _changed_timestamp_match(self, dependency, target, prev_ni, repo_node=None) -> bool:
        return dependency.changed_timestamp_match(target, prev_ni, repo_node)

    def Decider(self, function):
        self.cache_timestamp_newer = False
        if function in ('MD5', 'content'):
            # TODO: Handle if user requests MD5 and not content with deprecation notice
            function = self._changed_content
        elif function in ('MD5-timestamp', 'content-timestamp'):
            function = self._changed_timestamp_then_content
        elif function in ('timestamp-newer', 'make'):
            function = self._changed_timestamp_newer
            self.cache_timestamp_newer = True
        elif function == 'timestamp-match':
            function = self._changed_timestamp_match
        elif not callable(function):
            raise UserError("Unknown Decider value %s" % repr(function))

        # We don't use AddMethod because we don't want to turn the
        # function, which only expects three arguments, into a bound
        # method, which would add self as an initial, fourth argument.
        self.decide_target = function
        self.decide_source = function


    def Detect(self, progs):
        """Return the first available program from one or more possibilities.

        Args:
            progs (str or list): one or more command names to check for

        """
        if not is_List(progs):
            progs = [progs]
        for prog in progs:
            path = self.WhereIs(prog)
            if path: return prog
        return None


    def Dictionary(self, *args: str, as_dict: bool = False):
        """Return construction variables from an environment.

        Args:
          args (optional): construction variable names to select.
            If omitted, all variables are selected and returned
            as a dict.
          as_dict: if true, and *args* is supplied, return the
            variables and their values in a dict. If false
            (the default), return a single value as a scalar,
            or multiple values in a list.

        Returns:
          A dictionary of construction variables, or a single value
          or list of values.

        Raises:
          KeyError: if any of *args* is not in the construction environment.

        .. versionchanged:: 4.9.0
           Added the *as_dict* keyword arg to specify always returning a dict.
        """
        if not args:
            return self._dict
        if as_dict:
            return {key: self._dict[key] for key in args}
        dlist = [self._dict[key] for key in args]
        if len(dlist) == 1:
            return dlist[0]
        return dlist


    def Dump(self, *key: str, format: str = 'pretty') -> str:
        """Return string of serialized construction variables.

        Produces a "pretty" output of a dictionary of selected
        construction variables, or all of them. The display *format* is
        selectable. The result is intended for human consumption (e.g,
        to print), mainly when debugging.  Objects that cannot directly be
        represented get a placeholder like ``<function foo at 0x123456>``
        (pretty-print) or ``<<non-serializable: function>>`` (JSON).

        Args:
           key: if omitted, format the whole dict of variables,
              else format *key*(s) with the corresponding values.
           format: specify the format to serialize to. ``"pretty"`` generates
             a pretty-printed string, ``"json"`` a JSON-formatted string.

        Raises:
           ValueError: *format* is not a recognized serialization format.

        .. versionchanged:: 4.9.0
           *key* is no longer limited to a single construction variable name.
           If *key* is supplied, a formatted dictionary is generated like the
           no-arg case - previously a single *key* displayed just the value.
        """
        if len(key):
            cvars = self.Dictionary(*key, as_dict=True)
        else:
            cvars = self.Dictionary()

        fmt = format.lower()

        if fmt == 'pretty':
            import pprint  # pylint: disable=import-outside-toplevel

            pp = pprint.PrettyPrinter(indent=2)
            # TODO: pprint doesn't do a nice job on path-style values
            # if the paths contain spaces (i.e. Windows), because the
            # algorithm tries to break lines on spaces, while breaking
            # on the path-separator would be more "natural". Is there
            # a better way to format those?
            return pp.pformat(cvars)

        elif fmt == 'json':
            import json  # pylint: disable=import-outside-toplevel

            class DumpEncoder(json.JSONEncoder):
                """SCons special json Dump formatter."""

                def default(self, obj):
                    if isinstance(obj, (UserList, UserDict)):
                        return obj.data
                    return f'<<non-serializable: {type(obj).__qualname__}>>'

            return json.dumps(cvars, indent=4, cls=DumpEncoder, sort_keys=True)

        raise ValueError("Unsupported serialization format: %s." % fmt)


    def FindIxes(self, paths: Sequence[str], prefix: str, suffix: str) -> str | None:
        """Search *paths* for a path that has *prefix* and *suffix*.

        Returns on first match.

        Arguments:
           paths: the list of paths or nodes.
           prefix: construction variable for the prefix.
           suffix: construction variable for the suffix.

        Returns:
           The matched path or ``None``
        """
        suffix = self.subst('$'+suffix)
        prefix = self.subst('$'+prefix)

        for path in paths:
            name = os.path.basename(str(path))
            if name[:len(prefix)] == prefix and name[-len(suffix):] == suffix:
                return path


    def ParseConfig(self, command, function=None, unique: bool=True):
        """Parse the result of running a command to update construction vars.

        Use ``function`` to parse the output of running ``command``
        in order to modify the current environment.

        Args:
            command: a string or a list of strings representing a command
              and its arguments.
            function: called to process the result of ``command``, which will
              be passed as ``args``.  If ``function`` is omitted or ``None``,
              :meth:`MergeFlags` is used. Takes 3 args ``(env, args, unique)``
            unique: whether no duplicate values are allowed (default true)
        """
        if function is None:

            def parse_conf(env, cmd, unique=unique):
                return env.MergeFlags(cmd, unique)

            function = parse_conf
        if is_List(command):
            command = ' '.join(command)
        command = self.subst(command)
        return function(self, self.backtick(command), unique)


    def ParseDepends(self, filename, must_exist=None, only_one: bool=False):
        """
        Parse a mkdep-style file for explicit dependencies.  This is
        completely abusable, and should be unnecessary in the "normal"
        case of proper SCons configuration, but it may help make
        the transition from a Make hierarchy easier for some people
        to swallow.  It can also be genuinely useful when using a tool
        that can write a .d file, but for which writing a scanner would
        be too complicated.
        """
        filename = self.subst(filename)
        try:
            with open(filename) as fp:
                lines = LogicalLines(fp).readlines()
        except OSError:
            if must_exist:
                raise
            return
        lines = [l for l in lines if l[0] != '#']
        tdlist = []
        for line in lines:
            try:
                target, depends = line.split(':', 1)
            except (AttributeError, ValueError):
                # Throws AttributeError if line isn't a string.  Can throw
                # ValueError if line doesn't split into two or more elements.
                pass
            else:
                tdlist.append((target.split(), depends.split()))
        if only_one:
            targets = []
            for td in tdlist:
                targets.extend(td[0])
            if len(targets) > 1:
                raise UserError(
                            "More than one dependency target found in `%s':  %s"
                                            % (filename, targets))
        for target, depends in tdlist:
            self.Depends(target, depends)

    def Platform(self, platform):
        platform = self.subst(platform)
        return SCons.Platform.Platform(platform)(self)

    def Prepend(self, **kw) -> None:
        """Prepend values to construction variables in an Environment.

        The variable is created if it is not already present.
        """
        kw = copy_non_reserved_keywords(kw)
        for key, val in kw.items():
            if key == 'CPPDEFINES':
                _add_cppdefines(self._dict, val, prepend=True)
                continue
            try:
                orig = self._dict[key]
            except KeyError:
                # No existing var in the environment so set to the new value.
                self._dict[key] = val
                continue

            try:
                # Check if the original looks like a dict: has .update?
                update_dict = orig.update
            except AttributeError:
                try:
                    # Just try to add them together.  This will work
                    # in most cases, when the original and new values
                    # are compatible types.
                    self._dict[key] = val + orig
                except (KeyError, TypeError):
                    try:
                        # Check if the added value is a list: has .append?
                        add_to_val = val.append
                    except AttributeError:
                        # The added value isn't a list, but the
                        # original is (by process of elimination),
                        # so insert the the new value in the original
                        # (if there's one to insert).
                        if val:
                            orig.insert(0, val)
                    else:
                        # The added value is a list, so append
                        # the original to it (if there's a value
                        # to append) and replace the original.
                        if orig:
                            add_to_val(orig)
                        self._dict[key] = val
                continue

            # The original looks like a dictionary, so update it
            # based on what we think the value looks like.
            # We can't just try adding the value because
            # dictionaries don't have __add__() methods, and
            # things like UserList will incorrectly coerce the
            # original dict to a list (which we don't want).
            if is_List(val):
                for v in val:
                    orig[v] = None
            else:
                try:
                    update_dict(val)
                except (AttributeError, TypeError, ValueError):
                    if is_Dict(val):
                        for k, v in val.items():
                            orig[k] = v
                    else:
                        orig[val] = None

        self.scanner_map_delete(kw)

    def PrependENVPath(self, name, newpath, envname: str='ENV',
                       sep=os.pathsep, delete_existing: bool=True) -> None:
        """Prepend path elements to the path *name* in the *envname*
        dictionary for this environment.  Will only add any particular
        path once, and will normpath and normcase all paths to help
        assure this.  This can also handle the case where the env
        variable is a list instead of a string.

        If *delete_existing* is False, a *newpath* component already in the path
        will not be moved to the front (it will be left where it is).
        """

        orig = ''
        if envname in self._dict and name in self._dict[envname]:
            orig = self._dict[envname][name]

        nv = PrependPath(orig, newpath, sep, delete_existing,
                                    canonicalize=self._canonicalize)

        if envname not in self._dict:
            self._dict[envname] = {}

        self._dict[envname][name] = nv

    def PrependUnique(self, delete_existing: bool = False, **kw) -> None:
        """Prepend values uniquely to existing construction variables.

        Similar to :meth:`Prepend`, but the result may not contain duplicates
        of any values passed for each given key (construction variable),
        so an existing list may need to be pruned first, however it may still
        contain other duplicates.

        If *delete_existing* is true, removes existing values first, so values
        move to the front; otherwise (the default) values are skipped if
        already present.
        """
        kw = copy_non_reserved_keywords(kw)
        for key, val in kw.items():
            if key == 'CPPDEFINES':
                _add_cppdefines(self._dict, val, unique=True, prepend=True, delete_existing=delete_existing)
                continue
            if is_List(val):
                val = _delete_duplicates(val, not delete_existing)
            if key not in self._dict or self._dict[key] in ('', None):
                self._dict[key] = val
            elif is_Dict(self._dict[key]) and is_Dict(val):
                self._dict[key].update(val)
            elif is_List(val):
                dk = self._dict[key]
                if not is_List(dk):
                    dk = [dk]
                if delete_existing:
                    dk = [x for x in dk if x not in val]
                else:
                    val = [x for x in val if x not in dk]
                self._dict[key] = val + dk
            else:
                # val is not a list, so presumably a scalar (likely str).
                dk = self._dict[key]
                if is_List(dk):
                    if delete_existing:
                        dk = [x for x in dk if x != val]
                        self._dict[key] = [val] + dk
                    else:
                        if val not in dk:
                            self._dict[key] = [val] + dk
                else:
                    if delete_existing:
                        dk = [x for x in dk if x not in val]
                    self._dict[key] = val + dk
        self.scanner_map_delete(kw)

    def Replace(self, **kw) -> None:
        """Replace existing construction variables in an Environment
        with new construction variables and/or values.
        """
        try:
            kwbd = kw['BUILDERS']
        except KeyError:
            pass
        else:
            kwbd = BuilderDict(kwbd,self)
            del kw['BUILDERS']
            self.__setitem__('BUILDERS', kwbd)
        kw = copy_non_reserved_keywords(kw)
        self._update(semi_deepcopy(kw))
        self.scanner_map_delete(kw)

    def ReplaceIxes(self, path, old_prefix, old_suffix, new_prefix, new_suffix):
        """
        Replace old_prefix with new_prefix and old_suffix with new_suffix.

        env - Environment used to interpolate variables.
        path - the path that will be modified.
        old_prefix - construction variable for the old prefix.
        old_suffix - construction variable for the old suffix.
        new_prefix - construction variable for the new prefix.
        new_suffix - construction variable for the new suffix.
        """
        old_prefix = self.subst('$'+old_prefix)
        old_suffix = self.subst('$'+old_suffix)

        new_prefix = self.subst('$'+new_prefix)
        new_suffix = self.subst('$'+new_suffix)

        dir,name = os.path.split(str(path))
        if name[:len(old_prefix)] == old_prefix:
            name = name[len(old_prefix):]
        if name[-len(old_suffix):] == old_suffix:
            name = name[:-len(old_suffix)]
        return os.path.join(dir, new_prefix+name+new_suffix)

    def SetDefault(self, **kw) -> None:
        for k in list(kw.keys()):
            if k in self._dict:
                del kw[k]
        self.Replace(**kw)

    def _find_toolpath_dir(self, tp):
        return self.fs.Dir(self.subst(tp)).srcnode().get_abspath()

    def Tool(
        self, tool: str | Callable, toolpath: Collection[str] | None = None, **kwargs
    ) -> Callable:
        """Find and run tool module *tool*.

        *tool* is generally a string, but can also be a callable object,
        in which case it is just called, without any of the setup.
        The skipped setup includes storing *kwargs* into the created
        :class:`~SCons.Tool.Tool` instance, which is extracted and used
        when the instance is called, so in the skip case, the called
        object will not get the *kwargs*.

        .. versionchanged:: 4.2
           returns the tool object rather than ``None``.
        """
        if is_String(tool):
            tool = self.subst(tool)
            if toolpath is None:
                toolpath = self.get('toolpath', [])
            toolpath = list(map(self._find_toolpath_dir, toolpath))
            tool = SCons.Tool.Tool(tool, toolpath, **kwargs)
        tool(self)
        return tool

    def WhereIs(self, prog, path=None, pathext=None, reject=None):
        """Find prog in the path. """
        if not prog:  # nothing to search for, just give up
            return None
        if path is None:
            try:
                path = self['ENV']['PATH']
            except KeyError:
                pass
        elif is_String(path):
            path = self.subst(path)
        if pathext is None:
            try:
                pathext = self['ENV']['PATHEXT']
            except KeyError:
                pass
        elif is_String(pathext):
            pathext = self.subst(pathext)
        prog = CLVar(self.subst(prog))  # support "program --with-args"
        path = WhereIs(prog[0], path, pathext, reject)
        if path:
            return path
        return None

    #######################################################################
    # Public methods for doing real "SCons stuff" (manipulating
    # dependencies, setting attributes on targets, etc.).  These begin
    # with upper-case letters.  The essential characteristic of methods
    # in this section is that they all *should* have corresponding
    # same-named global functions.
    #######################################################################

    def Action(self, *args, **kw):
        def subst_string(a, self=self):
            if is_String(a):
                a = self.subst(a)
            return a
        nargs = list(map(subst_string, args))
        nkw = self.subst_kw(kw)
        return SCons.Action.Action(*nargs, **nkw)

    def AddPreAction(self, files, action):
        nodes = self.arg2nodes(files, self.fs.Entry)
        action = SCons.Action.Action(action)
        uniq = {}
        for executor in [n.get_executor() for n in nodes]:
            uniq[executor] = 1
        for executor in uniq.keys():
            executor.add_pre_action(action)
        return nodes

    def AddPostAction(self, files, action):
        nodes = self.arg2nodes(files, self.fs.Entry)
        action = SCons.Action.Action(action)
        uniq = {}
        for executor in [n.get_executor() for n in nodes]:
            uniq[executor] = 1
        for executor in uniq.keys():
            executor.add_post_action(action)
        return nodes

    def Alias(self, target, source=[], action=None, **kw):
        tlist = self.arg2nodes(target, self.ans.Alias)
        if not is_List(source):
            source = [source]
        source = [_f for _f in source if _f]

        if not action:
            if not source:
                # There are no source files and no action, so just
                # return a target list of classic Alias Nodes, without
                # any builder.  The externally visible effect is that
                # this will make the wrapping Script.BuildTask class
                # say that there's "Nothing to be done" for this Alias,
                # instead of that it's "up to date."
                return tlist

            # No action, but there are sources.  Re-call all the target
            # builders to add the sources to each target.
            result = []
            for t in tlist:
                bld = t.get_builder(AliasBuilder)
                result.extend(bld(self, t, source))
            return result

        nkw = self.subst_kw(kw)
        nkw.update({
            'action'            : SCons.Action.Action(action),
            'source_factory'    : self.fs.Entry,
            'multi'             : 1,
            'is_explicit'       : None,
        })
        bld = SCons.Builder.Builder(**nkw)

        # Apply the Builder separately to each target so that the Aliases
        # stay separate.  If we did one "normal" Builder call with the
        # whole target list, then all of the target Aliases would be
        # associated under a single Executor.
        result = []
        for t in tlist:
            # Calling the convert() method will cause a new Executor to be
            # created from scratch, so we have to explicitly initialize
            # it with the target's existing sources, plus our new ones,
            # so nothing gets lost.
            b = t.get_builder()
            if b is None or b is AliasBuilder:
                b = bld
            else:
                nkw['action'] = b.action + action
                b = SCons.Builder.Builder(**nkw)
            t.convert()
            result.extend(b(self, t, t.sources + source))
        return result

    def AlwaysBuild(self, *targets):
        tlist = []
        for t in targets:
            tlist.extend(self.arg2nodes(t, self.fs.Entry))
        for t in tlist:
            t.set_always_build()
        return tlist

    def Builder(self, **kw):
        nkw = self.subst_kw(kw)
        return SCons.Builder.Builder(**nkw)

    def CacheDir(self, path, custom_class=None) -> None:
        if path is not None:
            path = self.subst(path)
        self._CacheDir_path = path

        if custom_class:
            self['CACHEDIR_CLASS'] = self.validate_CacheDir_class(custom_class)

        if SCons.Action.execute_actions:
            # Only initialize the CacheDir if  -n/-no_exec was NOT specified.
            # Now initialized the CacheDir and prevent a race condition which can
            # happen when there's no existing cache dir and you are building with
            # multiple threads, but initializing it before the task walk starts
            self.get_CacheDir()

    def Clean(self, targets, files) -> None:
        """Mark additional files for cleaning.

        *files* will be removed if any of *targets* are selected
        for cleaning - that is, the combination of target selection
        and -c clean mode.

        Args:
            targets (files or nodes): targets to associate *files* with.
            files (files or nodes): items to remove if *targets* are selected.
        """
        global CleanTargets
        tlist = self.arg2nodes(targets, self.fs.Entry)
        flist = self.arg2nodes(files, self.fs.Entry)
        for t in tlist:
            try:
                CleanTargets[t].extend(flist)
            except KeyError:
                CleanTargets[t] = flist

    def Configure(self, *args, **kw):
        nargs = [self]
        if args:
            nargs = nargs + self.subst_list(args)[0]
        nkw = self.subst_kw(kw)
        nkw['_depth'] = kw.get('_depth', 0) + 1
        try:
            nkw['custom_tests'] = self.subst_kw(nkw['custom_tests'])
        except KeyError:
            pass
        return SCons.SConf.SConf(*nargs, **nkw)

    def Command(self, target, source, action, **kw):
        """Set up a one-off build command.

        Builds *target* from *source* using *action*, which may be
        be any type that the Builder factory will accept for an action.
        Generates an anonymous builder and calls it, to add the details
        to the build graph. The builder is not named, added to ``BUILDERS``,
        or otherwise saved.

        Recognizes the :func:`~SCons.Builder.Builder` keywords
        ``source_scanner``, ``target_scanner``, ``source_factory`` and
        ``target_factory``.  All other arguments from *kw* are passed on
        to the builder when it is called.
        """
        # Build a kwarg dict for the builder construction
        bkw = {
            'action': action,
            'target_factory': self.fs.Entry,
            'source_factory': self.fs.Entry,
        }

        # Recognize these kwargs for the builder construction and take
        # them out of the args for the subsequent builder call.
        for arg in [
            'source_scanner',
            'target_scanner',
            'source_factory',
            'target_factory',
        ]:
            try:
                bkw[arg] = kw.pop(arg)
            except KeyError:
                pass

        bld = SCons.Builder.Builder(**bkw)
        return bld(self, target, source, **kw)

    def Depends(self, target, dependency):
        """Explicity specify that *target* depends on *dependency*."""
        tlist = self.arg2nodes(target, self.fs.Entry)
        dlist = self.arg2nodes(dependency, self.fs.Entry)
        for t in tlist:
            t.add_dependency(dlist)
        return tlist

    def Dir(self, name, *args, **kw):
        """
        """
        s = self.subst(name)
        if is_Sequence(s):
            result=[]
            for e in s:
                result.append(self.fs.Dir(e, *args, **kw))
            return result
        return self.fs.Dir(s, *args, **kw)

    def PyPackageDir(self, modulename):
        s = self.subst(modulename)
        if is_Sequence(s):
            result=[]
            for e in s:
                result.append(self.fs.PyPackageDir(e))
            return result
        return self.fs.PyPackageDir(s)

    def NoClean(self, *targets) -> list:
        """Tag *targets* to not be removed in clean mode."""
        tlist = []
        for t in targets:
            tlist.extend(self.arg2nodes(t, self.fs.Entry))
        for t in tlist:
            t.set_noclean()
        return tlist

    def NoCache(self, *targets):
        """Tag target(s) so that it will not be cached."""
        tlist = []
        for t in targets:
            tlist.extend(self.arg2nodes(t, self.fs.Entry))
        for t in tlist:
            t.set_nocache()
        return tlist

    def Entry(self, name, *args, **kw):
        """
        """
        s = self.subst(name)
        if is_Sequence(s):
            result=[]
            for e in s:
                result.append(self.fs.Entry(e, *args, **kw))
            return result
        return self.fs.Entry(s, *args, **kw)

    def Environment(self, **kw):
        return SCons.Environment.Environment(**self.subst_kw(kw))

    def Execute(self, action, *args, **kw):
        """Directly execute an action through an Environment
        """
        action = self.Action(action, *args, **kw)
        result = action([], [], self)
        if isinstance(result, BuildError):
            errstr = result.errstr
            if result.filename:
                errstr = result.filename + ': ' + errstr
            sys.stderr.write("scons: *** %s\n" % errstr)
            return result.status
        else:
            return result

    def File(self, name, *args, **kw):
        """
        """
        s = self.subst(name)
        if is_Sequence(s):
            result=[]
            for e in s:
                result.append(self.fs.File(e, *args, **kw))
            return result
        return self.fs.File(s, *args, **kw)

    def FindFile(self, file, dirs):
        file = self.subst(file)
        nodes = self.arg2nodes(dirs, self.fs.Dir)
        return SCons.Node.FS.find_file(file, tuple(nodes))

    def Flatten(self, sequence):
        return flatten(sequence)

    def GetBuildPath(self, files):
        result = list(map(str, self.arg2nodes(files, self.fs.Entry)))
        if is_List(files):
            return result
        else:
            return result[0]

    def Glob(self, pattern, ondisk: bool=True, source: bool=False, strings: bool=False, exclude=None):
        return self.fs.Glob(self.subst(pattern), ondisk, source, strings, exclude)

    def Ignore(self, target, dependency):
        """Ignore a dependency."""
        tlist = self.arg2nodes(target, self.fs.Entry)
        dlist = self.arg2nodes(dependency, self.fs.Entry)
        for t in tlist:
            t.add_ignore(dlist)
        return tlist

    def Literal(self, string):
        return SCons.Subst.Literal(string)

    def Local(self, *targets):
        ret = []
        for targ in targets:
            if isinstance(targ, SCons.Node.Node):
                targ.set_local()
                ret.append(targ)
            else:
                for t in self.arg2nodes(targ, self.fs.Entry):
                   t.set_local()
                   ret.append(t)
        return ret

    def Precious(self, *targets):
        """Mark *targets* as precious: do not delete before building."""
        tlist = []
        for t in targets:
            tlist.extend(self.arg2nodes(t, self.fs.Entry))
        for t in tlist:
            t.set_precious()
        return tlist

    def Pseudo(self, *targets):
        """Mark *targets* as pseudo: must not exist."""
        tlist = []
        for t in targets:
            tlist.extend(self.arg2nodes(t, self.fs.Entry))
        for t in tlist:
            t.set_pseudo()
        return tlist

    def Repository(self, *dirs, **kw) -> None:
        """Specify Repository directories to search."""
        dirs = self.arg2nodes(list(dirs), self.fs.Dir)
        self.fs.Repository(*dirs, **kw)

    def Requires(self, target, prerequisite):
        """Specify that *prerequisite* must be built before *target*.

        Creates an order-only relationship, not a full dependency.
        *prerequisite* must exist before *target* can be built, but
        a change to *prerequisite* does not trigger a rebuild of *target*.
        """
        tlist = self.arg2nodes(target, self.fs.Entry)
        plist = self.arg2nodes(prerequisite, self.fs.Entry)
        for t in tlist:
            t.add_prerequisite(plist)
        return tlist

    def Scanner(self, *args, **kw):
        nargs = []
        for arg in args:
            if is_String(arg):
                arg = self.subst(arg)
            nargs.append(arg)
        nkw = self.subst_kw(kw)
        return SCons.Scanner.ScannerBase(*nargs, **nkw)

    def SConsignFile(self, name=SCons.SConsign.current_sconsign_filename(), dbm_module=None) -> None:
        if name is not None:
            name = self.subst(name)
            if not os.path.isabs(name):
                name = os.path.join(str(self.fs.SConstruct_dir), name)
        if name:
            name = os.path.normpath(name)
            sconsign_dir = os.path.dirname(name)
            if sconsign_dir and not os.path.exists(sconsign_dir):
                self.Execute(SCons.Defaults.Mkdir(sconsign_dir))
        SCons.SConsign.File(name, dbm_module)

    def SideEffect(self, side_effect, target):
        """Tell scons that side_effects are built as side
        effects of building targets."""
        side_effects = self.arg2nodes(side_effect, self.fs.Entry)
        targets = self.arg2nodes(target, self.fs.Entry)

        added_side_effects = []
        for side_effect in side_effects:
            if side_effect.multiple_side_effect_has_builder():
                raise UserError("Multiple ways to build the same target were specified for: %s" % str(side_effect))
            side_effect.add_source(targets)
            side_effect.side_effect = 1
            self.Precious(side_effect)
            added = False
            for target in targets:
                if side_effect not in target.side_effects:
                    target.side_effects.append(side_effect)
                    added = True
            if added:
                added_side_effects.append(side_effect)
        return added_side_effects

    def Split(self, arg):
        """This function converts a string or list into a list of strings
        or Nodes.  This makes things easier for users by allowing files to
        be specified as a white-space separated list to be split.

        The input rules are:
            - A single string containing names separated by spaces. These will be
              split apart at the spaces.
            - A single Node instance
            - A list containing either strings or Node instances. Any strings
              in the list are not split at spaces.

        In all cases, the function returns a list of Nodes and strings."""

        if is_List(arg):
            return list(map(self.subst, arg))
        elif is_String(arg):
            return self.subst(arg).split()
        else:
            return [self.subst(arg)]

    def Value(self, value, built_value=None, name=None):
        """Return a Value (Python expression) node.

        .. versionchanged:: 4.0
           the *name* parameter was added.
        """
        return SCons.Node.Python.ValueWithMemo(value, built_value, name)

    def VariantDir(self, variant_dir, src_dir, duplicate: int=1) -> None:
        variant_dir = self.arg2nodes(variant_dir, self.fs.Dir)[0]
        src_dir = self.arg2nodes(src_dir, self.fs.Dir)[0]
        self.fs.VariantDir(variant_dir, src_dir, duplicate)

    def FindSourceFiles(self, node: str='.') -> list:
        """Return a list of all source files."""
        node = self.arg2nodes(node, self.fs.Entry)[0]

        sources = []
        def build_source(ss) -> None:
            for s in ss:
                if isinstance(s, SCons.Node.FS.Dir):
                    build_source(s.all_children())
                elif s.has_builder():
                    build_source(s.sources)
                elif isinstance(s.disambiguate(), SCons.Node.FS.File):
                    sources.append(s)
        build_source(node.all_children())

        def final_source(node):
            while node != node.srcnode():
              node = node.srcnode()
            return node
        sources = list(map(final_source, sources))
        # remove duplicates
        return list(set(sources))

    def FindInstalledFiles(self):
        """ returns the list of all targets of the Install and InstallAs Builder.
        """
        from SCons.Tool import install
        if install._UNIQUE_INSTALLED_FILES is None:
            install._UNIQUE_INSTALLED_FILES = uniquer_hashables(install._INSTALLED_FILES)
        return install._UNIQUE_INSTALLED_FILES


class OverrideEnvironment(Base):
    """A proxy that implements override environments.

    Returns attributes/methods and construction variables from the
    base environment *subject*, except that same-named construction
    variables from *overrides* are returned on read access; assignment
    to a construction variable creates an override entry - *subject* is
    not modified. This is a much lighter weight approach for limited-use
    setups than cloning an environment, for example to handle a builder
    call with keyword arguments that make a temporary change to the
    current environment::

        env.Program(target="foo", source=sources, DEBUG=True)

    While the majority of methods are proxied from the underlying environment
    class, enough plumbing is defined in this class for it to behave
    like an ordinary Environment without the caller needing to know it is
    "special" in some way.  We don't call the initializer of the class
    we're proxying, rather depend on it already being properly set up.

    Deletion is handled specially, if a variable was explicitly deleted,
    it should no longer appear to be in the env, but we also don't want to
    modify the subject environment.

    :class:`OverrideEnvironment` can nest arbitratily, *subject*
    can be an existing instance. Although instances can be
    instantiated directly, the expected use is to call the
    :meth:`~SubstitutionEnvironment.Override` method as a factory.

    Note Python does not give us a way to assure the subject environment
    is not modified. Assigning to a variable creates a new entry in
    the override, but moditying a variable will first fetch the one
    from the subject, and if mutable, it will just be modified in place.
    For example: ``over_env.Append(CPPDEFINES="-O")``, where ``CPPDEFINES``
    is an existing list or :class:`~SCons.Util.CLVar`, will successfully
    append to ``CPPDEFINES`` in the subject env.  To avoid such leakage,
    clients such as Scanners, Emitters and Action functions called by a
    Builder using override syntax must take care if modifying an env
    (which is not advised anyway) in case they were passed an
    ``OverrideEnvironment``.
    """

    def __init__(self, subject, overrides: dict | None = None) -> None:
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Environment.OverrideEnvironment')
        overrides = {} if overrides is None else overrides
        # set these directly via __dict__ to avoid trapping by __setattr__
        self.__dict__['__subject'] = subject
        self.__dict__['overrides'] = overrides
        self.__dict__['__deleted'] = []

    # Methods that make this class act like a proxy.

    def __getattr__(self, name):
        # Proxied environment methods don't know (nor should they have to) that
        # they could be called with an OverrideEnvironment as 'self' and may
        # access the _dict construction variable dict directly, so we need to
        # pretend to have one, and not serve up the one from the subject, or it
        # will miss the overridden values (and possibly modify the base). Use
        # ourselves and hope the dict-like methods below are sufficient.
        if name == '_dict':
            return self

        attr = getattr(self.__dict__['__subject'], name)

        # Check first if attr is one of the Wrapper classes, for example
        # when a pseudo-builder is being called from an OverrideEnvironment.
        # These wrappers, when they're constructed, capture the Environment
        # they are being constructed with and so will not have access to
        # overridden values. So we rebuild them with the OverrideEnvironment
        # so they have access to overridden values.
        if isinstance(attr, MethodWrapper):
            return attr.clone(self)
        else:
            return attr

    def __setattr__(self, name, value) -> None:
        setattr(self.__dict__['__subject'], name, value)

    # Methods that make this class act like a dictionary.

    def __getitem__(self, key):
        """Return the visible value of *key*.

        Backfills from the subject env if *key* doesn't have an entry in
        the override, and is not explicity deleted.
        """
        try:
            return self.__dict__['overrides'][key]
        except KeyError:
            if key in self.__dict__['__deleted']:
                raise
            return self.__dict__['__subject'].__getitem__(key)

    def __setitem__(self, key, value) -> None:
        # This doesn't have the same performance equation as a "real"
        # environment: in an override you're basically just writing
        # new stuff; it's not a common case to be changing values already
        # set in the override dict, so don't spend time checking for existance.
        if not key.isidentifier():
            raise UserError(f"Illegal construction variable {key!r}")
        self.__dict__['overrides'][key] = value
        if key in self.__dict__['__deleted']:
            # it's no longer "deleted" if we set it
            self.__dict__['__deleted'].remove(key)

    def __delitem__(self, key) -> None:
        """Delete *key* from override.

        Makes *key* not visible in the override. Previously implemented
        by deleting from ``overrides`` and from ``__subject``, which
        keeps :meth:`__getitem__` from filling  it back in next time.
        However, that approach was a form of leak, as the subject
        environment was modified. So instead we log that it's deleted
        and use that to make decisions elsewhere.
        """
        try:
            del self.__dict__['overrides'][key]
        except KeyError:
            deleted = False
        else:
            deleted = True
        if not deleted and key not in self.__dict__['__subject']:
            raise KeyError(key)
        self.__dict__['__deleted'].append(key)

    def get(self, key, default=None):
        """Emulates the ``get`` method of dictionaries.

        Backfills from the subject environment if *key* is not in the override
        and not deleted.
        """
        try:
            return self.__dict__['overrides'][key]
        except KeyError:
            if key in self.__dict__['__deleted']:
                return default
            return self.__dict__['__subject'].get(key, default)

    def __contains__(self, key) -> bool:
        """Emulates the ``contains`` method of dictionaries.

        Backfills from the subject environment if *key* is not in the override
        and not deleted.
        """
        if key in self.__dict__['overrides']:
            return True
        if key in self.__dict__['__deleted']:
            return False
        return key in self.__dict__['__subject']

    def Dictionary(self, *args, as_dict: bool = False):
        """Return construction variables from an environment.

        Behavior is as described for :class:`SubstitutionEnvironment.Dictionary`
        but understanda about the override.

        Raises:
          KeyError: if any of *args* is not in the construction environment.

        .. versionchanged: 4.9.0
           Added the *as_dict* keyword arg to always return a dict.
        """
        d = {}
        d.update(self.__dict__['__subject'])
        d.update(self.__dict__['overrides'])
        d = {k: v for k, v in d.items() if k not in self.__dict__['__deleted']}
        if not args:
            return d
        if as_dict:
            return {key: d[key] for key in args}
        dlist = [d[key] for key in args]
        if len(dlist) == 1:
            return dlist[0]
        return dlist

    def items(self):
        """Emulates the ``items`` method of dictionaries."""
        return self.Dictionary().items()

    def keys(self):
        """Emulates the ``keys`` method of dictionaries."""
        return self.Dictionary().keys()

    def values(self):
        """Emulates the ``values`` method of dictionaries."""
        return self.Dictionary().values()

    def setdefault(self, key, default=None):
        """Emulates the ``setdefault`` method of dictionaries."""
        try:
            return self.__getitem__(key)
        except KeyError:
            self.__dict__['overrides'][key] = default
            return default

    # Overridden private construction environment methods.

    def _update(self, other) -> None:
        self.__dict__['overrides'].update(other)

    def _update_onlynew(self, other) -> None:
        """Update a dict with new keys.

        Unlike the .update method, if the key is already present,
        it is not replaced.
        """
        for k, v in other.items():
            if k not in self.__dict__['overrides']:
                self.__dict__['overrides'][k] = v

    def gvars(self):
        return self.__dict__['__subject'].gvars()

    def lvars(self):
        lvars = self.__dict__['__subject'].lvars()
        lvars.update(self.__dict__['overrides'])
        return lvars

    # Overridden public construction environment methods.

    def Replace(self, **kw) -> None:
        kw = copy_non_reserved_keywords(kw)
        self.__dict__['overrides'].update(semi_deepcopy(kw))


# The entry point that will be used by the external world
# to refer to a construction environment.  This allows the wrapper
# interface to extend a construction environment for its own purposes
# by subclassing SCons.Environment.Base and then assigning the
# class to SCons.Environment.Environment.

Environment = Base


def NoSubstitutionProxy(subject):
    """
    An entry point for returning a proxy subclass instance that overrides
    the subst*() methods so they don't actually perform construction
    variable substitution.  This is specifically intended to be the shim
    layer in between global function calls (which don't want construction
    variable substitution) and the DefaultEnvironment() (which would
    substitute variables if left to its own devices).

    We have to wrap this in a function that allows us to delay definition of
    the class until it's necessary, so that when it subclasses Environment
    it will pick up whatever Environment subclass the wrapper interface
    might have assigned to SCons.Environment.Environment.
    """
    class _NoSubstitutionProxy(Environment):
        def __init__(self, subject) -> None:
            self.__dict__['__subject'] = subject

        def __getattr__(self, name):
            return getattr(self.__dict__['__subject'], name)

        def __setattr__(self, name, value):
            return setattr(self.__dict__['__subject'], name, value)

        def executor_to_lvars(self, kwdict) -> None:
            if 'executor' in kwdict:
                kwdict['lvars'] = kwdict['executor'].get_lvars()
                del kwdict['executor']
            else:
                kwdict['lvars'] = {}

        def raw_to_mode(self, mapping) -> None:
            try:
                raw = mapping['raw']
            except KeyError:
                pass
            else:
                del mapping['raw']
                mapping['mode'] = raw

        def subst(self, string, *args, **kwargs):
            return string

        def subst_kw(self, kw, *args, **kwargs):
            return kw

        def subst_list(self, string, *args, **kwargs):
            nargs = (string, self,) + args
            nkw = kwargs.copy()
            nkw['gvars'] = {}
            self.executor_to_lvars(nkw)
            self.raw_to_mode(nkw)
            return SCons.Subst.scons_subst_list(*nargs, **nkw)

        def subst_target_source(self, string, *args, **kwargs):
            nargs = (string, self,) + args
            nkw = kwargs.copy()
            nkw['gvars'] = {}
            self.executor_to_lvars(nkw)
            self.raw_to_mode(nkw)
            return SCons.Subst.scons_subst(*nargs, **nkw)

    return _NoSubstitutionProxy(subject)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
