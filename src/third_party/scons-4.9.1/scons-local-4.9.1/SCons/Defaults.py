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
#

"""Builders and other things for the local site.

Here's where we'll duplicate the functionality of autoconf until we
move it into the installation procedure or use something like qmconf.

The code that reads the registry to find MSVC components was borrowed
from distutils.msvccompiler.
"""

from __future__ import annotations

import os
import shutil
import stat
import sys
import time
from typing import Callable

import SCons.Action
import SCons.Builder
import SCons.CacheDir
import SCons.Environment
import SCons.Errors
import SCons.PathList
import SCons.Scanner.Dir
import SCons.Subst
import SCons.Tool
from SCons.Util import is_List, is_String, is_Sequence, is_Tuple, is_Dict, flatten

# A placeholder for a default Environment (for fetching source files
# from source code management systems and the like).  This must be
# initialized later, after the top-level directory is set by the calling
# interface.
_default_env = None


# Lazily instantiate the default environment so the overhead of creating
# it doesn't apply when it's not needed.
def _fetch_DefaultEnvironment(*args, **kwargs):
    """Returns the already-created default construction environment."""
    return _default_env


def DefaultEnvironment(*args, **kwargs):
    """Construct the global ("default") construction environment.

    The environment is provisioned with the values from *kwargs*.

    After the environment is created, this function is replaced with
    a reference to :func:`_fetch_DefaultEnvironment` which efficiently
    returns the initialized default construction environment without
    checking for its existence.

    Historically, some parts of the code held references to this function.
    Thus it still has the existence check for :data:`_default_env` rather
    than just blindly creating the environment and overwriting itself.
    """
    global _default_env
    if not _default_env:
        _default_env = SCons.Environment.Environment(*args, **kwargs)
        _default_env.Decider('content')
        global DefaultEnvironment
        DefaultEnvironment = _fetch_DefaultEnvironment
        _default_env._CacheDir_path = None
    return _default_env


# Emitters for setting the shared attribute on object files,
# and an action for checking that all of the source files
# going into a shared library are, in fact, shared.
def StaticObjectEmitter(target, source, env):
    for tgt in target:
        tgt.attributes.shared = False
    return target, source


def SharedObjectEmitter(target, source, env):
    for tgt in target:
        tgt.attributes.shared = 1
    return target, source


def SharedFlagChecker(source, target, env):
    same = env.subst('$STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME')
    if same == '0' or same == '' or same == 'False':
        for src in source:
            try:
                shared = src.attributes.shared
            except AttributeError:
                shared = False
            if not shared:
                raise SCons.Errors.UserError(
                    "Source file: %s is static and is not compatible with shared target: %s" % (src, target[0]))


SharedCheck = SCons.Action.Action(SharedFlagChecker, None)

# Some people were using these variable name before we made
# SourceFileScanner part of the public interface.  Don't break their
# SConscript files until we've given them some fair warning and a
# transition period.
CScan = SCons.Tool.CScanner
DScan = SCons.Tool.DScanner
LaTeXScan = SCons.Tool.LaTeXScanner
ObjSourceScan = SCons.Tool.SourceFileScanner
ProgScan = SCons.Tool.ProgramScanner

# These aren't really tool scanners, so they don't quite belong with
# the rest of those in Tool/__init__.py, but I'm not sure where else
# they should go.  Leave them here for now.

DirScanner = SCons.Scanner.Dir.DirScanner()
DirEntryScanner = SCons.Scanner.Dir.DirEntryScanner()

# Actions for common languages.
CAction = SCons.Action.Action("$CCCOM", "$CCCOMSTR")
ShCAction = SCons.Action.Action("$SHCCCOM", "$SHCCCOMSTR")
CXXAction = SCons.Action.Action("$CXXCOM", "$CXXCOMSTR")
ShCXXAction = SCons.Action.Action("$SHCXXCOM", "$SHCXXCOMSTR")

DAction = SCons.Action.Action("$DCOM", "$DCOMSTR")
ShDAction = SCons.Action.Action("$SHDCOM", "$SHDCOMSTR")

ASAction = SCons.Action.Action("$ASCOM", "$ASCOMSTR")
ASPPAction = SCons.Action.Action("$ASPPCOM", "$ASPPCOMSTR")

LinkAction = SCons.Action.Action("$LINKCOM", "$LINKCOMSTR")
ShLinkAction = SCons.Action.Action("$SHLINKCOM", "$SHLINKCOMSTR")

LdModuleLinkAction = SCons.Action.Action("$LDMODULECOM", "$LDMODULECOMSTR")

# Common tasks that we allow users to perform in platform-independent
# ways by creating ActionFactory instances.
ActionFactory = SCons.Action.ActionFactory


def get_paths_str(dest) -> str:
    """Generates a string from *dest* for use in a strfunction.

    If *dest* is a list, manually converts each elem to a string.
    """
    def quote(arg) -> str:
        return f'"{arg}"'

    if is_List(dest):
        elem_strs = [quote(d) for d in dest]
        return f'[{", ".join(elem_strs)}]'
    else:
        return quote(dest)


permission_dic = {
    'u': {
        'r': stat.S_IRUSR,
        'w': stat.S_IWUSR,
        'x': stat.S_IXUSR
    },
    'g': {
        'r': stat.S_IRGRP,
        'w': stat.S_IWGRP,
        'x': stat.S_IXGRP
    },
    'o': {
        'r': stat.S_IROTH,
        'w': stat.S_IWOTH,
        'x': stat.S_IXOTH
    }
}


def chmod_func(dest, mode) -> None:
    """Implementation of the Chmod action function.

    *mode* can be either an integer (normally expressed in octal mode,
    as in 0o755) or a string following the syntax of the POSIX chmod
    command (for example "ugo+w"). The latter must be converted, since
    the underlying Python only takes the numeric form.
    """
    from string import digits
    SCons.Node.FS.invalidate_node_memos(dest)
    if not is_List(dest):
        dest = [dest]
    if is_String(mode) and 0 not in [i in digits for i in mode]:
        mode = int(mode, 8)
    if not is_String(mode):
        for element in dest:
            os.chmod(str(element), mode)
    else:
        mode = str(mode)
        for operation in mode.split(","):
            if "=" in operation:
                operator = "="
            elif "+" in operation:
                operator = "+"
            elif "-" in operation:
                operator = "-"
            else:
                raise SyntaxError("Could not find +, - or =")
            operation_list = operation.split(operator)
            if len(operation_list) != 2:
                raise SyntaxError("More than one operator found")
            user = operation_list[0].strip().replace("a", "ugo")
            permission = operation_list[1].strip()
            new_perm = 0
            for u in user:
                for p in permission:
                    try:
                        new_perm = new_perm | permission_dic[u][p]
                    except KeyError:
                        raise SyntaxError("Unrecognized user or permission format")
            for element in dest:
                curr_perm = os.stat(str(element)).st_mode
                if operator == "=":
                    os.chmod(str(element), new_perm)
                elif operator == "+":
                    os.chmod(str(element), curr_perm | new_perm)
                elif operator == "-":
                    os.chmod(str(element), curr_perm & ~new_perm)


def chmod_strfunc(dest, mode) -> str:
    """strfunction for the Chmod action function."""
    if not is_String(mode):
        return f'Chmod({get_paths_str(dest)}, {mode:#o})'
    else:
        return f'Chmod({get_paths_str(dest)}, "{mode}")'



Chmod = ActionFactory(chmod_func, chmod_strfunc)



def copy_func(dest, src, symlinks: bool=True) -> int:
    """Implementation of the Copy action function.

    Copies *src* to *dest*.  If *src* is a list, *dest* must be
    a directory, or not exist (will be created).

    Since Python :mod:`shutil` methods, which know nothing about
    SCons Nodes, will be called to perform the actual copying,
    args are converted to strings first.

    If *symlinks* evaluates true, then a symbolic link will be
    shallow copied and recreated as a symbolic link; otherwise, copying
    a symbolic link will be equivalent to copying the symbolic link's
    final target regardless of symbolic link depth.
    """

    dest = str(dest)
    src = [str(n) for n in src] if is_List(src) else str(src)

    SCons.Node.FS.invalidate_node_memos(dest)
    if is_List(src):
        # this fails only if dest exists and is not a dir
        try:
            os.makedirs(dest, exist_ok=True)
        except FileExistsError:
            raise SCons.Errors.BuildError(
                errstr=(
                    'Error: Copy() called with a list of sources, '
                    'which requires target to be a directory, '
                    f'but "{dest}" is not a directory.'
                )
            )
        for file in src:
            shutil.copy2(file, dest)
        return 0

    elif os.path.islink(src):
        if symlinks:
            try:
                os.symlink(os.readlink(src), dest)
            except FileExistsError:
                raise SCons.Errors.BuildError(
                    errstr=(
                        f'Error: Copy() called to create symlink at "{dest}",'
                        ' but a file already exists at that location.'
                    )
                )
            return 0

        return copy_func(dest, os.path.realpath(src))

    elif os.path.isfile(src):
        shutil.copy2(src, dest)
        return 0

    else:
        shutil.copytree(src, dest, symlinks)
        return 0


def copy_strfunc(dest, src, symlinks: bool=True) -> str:
    """strfunction for the Copy action function."""
    return f'Copy({get_paths_str(dest)}, {get_paths_str(src)})'


Copy = ActionFactory(copy_func, copy_strfunc)


def delete_func(dest, must_exist: bool=False) -> None:
    """Implementation of the Delete action function.

    Lets the Python :func:`os.unlink` raise an error if *dest* does not exist,
    unless *must_exist* evaluates false (the default).
    """
    SCons.Node.FS.invalidate_node_memos(dest)
    if not is_List(dest):
        dest = [dest]
    for entry in dest:
        entry = str(entry)
        # os.path.exists returns False with broken links that exist
        entry_exists = os.path.exists(entry) or os.path.islink(entry)
        if not entry_exists and not must_exist:
            continue
        # os.path.isdir returns True when entry is a link to a dir
        if os.path.isdir(entry) and not os.path.islink(entry):
            shutil.rmtree(entry, True)
            continue
        os.unlink(entry)


def delete_strfunc(dest, must_exist: bool=False) -> str:
    """strfunction for the Delete action function."""
    return f'Delete({get_paths_str(dest)})'


Delete = ActionFactory(delete_func, delete_strfunc)


def mkdir_func(dest) -> None:
    """Implementation of the Mkdir action function."""
    SCons.Node.FS.invalidate_node_memos(dest)
    if not is_List(dest):
        dest = [dest]
    for entry in dest:
        os.makedirs(str(entry), exist_ok=True)


Mkdir = ActionFactory(mkdir_func, lambda _dir: f'Mkdir({get_paths_str(_dir)})')


def move_func(dest, src) -> None:
    """Implementation of the Move action function."""
    SCons.Node.FS.invalidate_node_memos(dest)
    SCons.Node.FS.invalidate_node_memos(src)
    shutil.move(src, dest)


Move = ActionFactory(
    move_func, lambda dest, src: f'Move("{dest}", "{src}")', convert=str
)


def touch_func(dest) -> None:
    """Implementation of the Touch action function."""
    SCons.Node.FS.invalidate_node_memos(dest)
    if not is_List(dest):
        dest = [dest]
    for file in dest:
        file = str(file)
        mtime = int(time.time())
        if os.path.exists(file):
            atime = os.path.getatime(file)
        else:
            with open(file, 'w'):
                atime = mtime
        os.utime(file, (atime, mtime))


Touch = ActionFactory(touch_func, lambda file: f'Touch({get_paths_str(file)})')


# Internal utility functions

# pylint: disable-msg=too-many-arguments
def _concat(prefix, items_iter, suffix, env, f=lambda x: x, target=None, source=None, affect_signature: bool=True):
    """
    Creates a new list from 'items_iter' by first interpolating each element
    in the list using the 'env' dictionary and then calling f on the
    list, and finally calling _concat_ixes to concatenate 'prefix' and
    'suffix' onto each element of the list.
    """

    if not items_iter:
        return items_iter

    l = f(SCons.PathList.PathList(items_iter).subst_path(env, target, source))
    if l is not None:
        items_iter = l

    if not affect_signature:
        value = ['$(']
    else:
        value = []
    value += _concat_ixes(prefix, items_iter, suffix, env)

    if not affect_signature:
        value += ["$)"]

    return value
# pylint: enable-msg=too-many-arguments


def _concat_ixes(prefix, items_iter, suffix, env):
    """
    Creates a new list from 'items_iter' by concatenating the 'prefix' and
    'suffix' arguments onto each element of the list.  A trailing space
    on 'prefix' or leading space on 'suffix' will cause them to be put
    into separate list elements rather than being concatenated.
    """

    result = []

    # ensure that prefix and suffix are strings
    prefix = str(env.subst(prefix, SCons.Subst.SUBST_RAW))
    suffix = str(env.subst(suffix, SCons.Subst.SUBST_RAW))

    for x in flatten(items_iter):
        if isinstance(x, SCons.Node.FS.File):
            result.append(x)
            continue
        x = str(x)
        if x:

            if prefix:
                if prefix[-1] == ' ':
                    result.append(prefix[:-1])
                elif x[:len(prefix)] != prefix:
                    x = prefix + x

            result.append(x)

            if suffix:
                if suffix[0] == ' ':
                    result.append(suffix[1:])
                elif x[-len(suffix):] != suffix:
                    result[-1] = result[-1] + suffix

    return result


def _stripixes(
    prefix: str,
    items,
    suffix: str,
    stripprefixes: list[str],
    stripsuffixes: list[str],
    env,
    literal_prefix: str = "",
    c: Callable[[list], list] = None,
) -> list:
    """Returns a list with text added to items after first stripping them.

    A companion to :func:`_concat_ixes`, used by tools (like the GNU
    linker) that need to turn something like ``libfoo.a`` into ``-lfoo``.
    *stripprefixes* and *stripsuffixes* are stripped from *items*.
    Calls function *c* to postprocess the result.

    Args:
        prefix: string to prepend to elements
        items: string or iterable to transform
        suffix: string to append to elements
        stripprefixes: prefix string(s) to strip from elements
        stripsuffixes: suffix string(s) to strip from elements
        env: construction environment for variable interpolation
        c: optional function to perform a transformation on the list.
           The default is `None`, which will select :func:`_concat_ixes`.
    """
    if not items:
        return items

    if not callable(c):
        env_c = env['_concat']
        if env_c != _concat and callable(env_c):
            # There's a custom _concat() method in the construction
            # environment, and we've allowed people to set that in
            # the past (see test/custom-concat.py), so preserve the
            # backwards compatibility.
            c = env_c
        else:
            c = _concat_ixes

    stripprefixes = list(map(env.subst, flatten(stripprefixes)))
    stripsuffixes = list(map(env.subst, flatten(stripsuffixes)))

    # This is a little funky: if literal_prefix is the same as os.pathsep
    # (e.g. both ':'), the normal conversion to a PathList will drop the
    # literal_prefix prefix. Tell it not to split in that case, which *should*
    # be okay because if we come through here, we're normally processing
    # library names and won't have strings like "path:secondpath:thirdpath"
    # which is why PathList() otherwise wants to split strings.
    do_split = not literal_prefix == os.pathsep

    stripped = []
    for l in SCons.PathList.PathList(items, do_split).subst_path(env, None, None):
        if isinstance(l, SCons.Node.FS.File):
            stripped.append(l)
            continue

        if not is_String(l):
            l = str(l)

        if literal_prefix and l.startswith(literal_prefix):
            stripped.append(l)
            continue

        for stripprefix in stripprefixes:
            lsp = len(stripprefix)
            if l[:lsp] == stripprefix:
                l = l[lsp:]
                # Do not strip more than one prefix
                break

        for stripsuffix in stripsuffixes:
            lss = len(stripsuffix)
            if l[-lss:] == stripsuffix:
                l = l[:-lss]
                # Do not strip more than one suffix
                break

        stripped.append(l)

    return c(prefix, stripped, suffix, env)


def processDefines(defs) -> list[str]:
    """Return list of strings for preprocessor defines from *defs*.

    Resolves the different forms ``CPPDEFINES`` can be assembled in:
    if the Append/Prepend routines are used beyond a initial setting it
    will be a deque, but if written to only once (Environment initializer,
    or direct write) it can be a multitude of types.

    Any prefix/suffix is handled elsewhere (usually :func:`_concat_ixes`).

    .. versionchanged:: 4.5.0
       Bare tuples are now treated the same as tuple-in-sequence, assumed
       to describe a valued macro. Bare strings are now split on space.
       A dictionary is no longer sorted before handling.
    """
    dlist = []
    if is_List(defs):
        for define in defs:
            if define is None:
                continue
            elif is_Sequence(define):
                if len(define) > 2:
                    raise SCons.Errors.UserError(
                        f"Invalid tuple in CPPDEFINES: {define!r}, "
                        "must be a tuple with only two elements"
                    )
                name, *value = define
                if value and value[0] is not None:
                    # TODO: do we need to quote value if it contains space?
                    dlist.append(f"{name}={value[0]}")
                else:
                    dlist.append(str(define[0]))
            elif is_Dict(define):
                for macro, value in define.items():
                    if value is not None:
                        # TODO: do we need to quote value if it contains space?
                        dlist.append(f"{macro}={value}")
                    else:
                        dlist.append(str(macro))
            elif is_String(define):
                dlist.append(str(define))
            else:
                raise SCons.Errors.UserError(
                    f"CPPDEFINES entry {define!r} is not a tuple, list, "
                    "dict, string or None."
                )
    elif is_Tuple(defs):
        if len(defs) > 2:
            raise SCons.Errors.UserError(
                f"Invalid tuple in CPPDEFINES: {defs!r}, "
                "must be a tuple with only two elements"
            )
        name, *value = defs
        if value and value[0] is not None:
            # TODO: do we need to quote value if it contains space?
            dlist.append(f"{name}={value[0]}")
        else:
            dlist.append(str(define[0]))
    elif is_Dict(defs):
        for macro, value in defs.items():
            if value is None:
                dlist.append(str(macro))
            else:
                dlist.append(f"{macro}={value}")
    elif is_String(defs):
        return defs.split()
    else:
        dlist.append(str(defs))

    return dlist


def _defines(prefix, defs, suffix, env, target=None, source=None, c=_concat_ixes):
    """A wrapper around :func:`_concat_ixes` that turns a list or string
    into a list of C preprocessor command-line definitions.
    """
    return c(prefix, env.subst_list(processDefines(defs), target=target, source=source), suffix, env)


class NullCmdGenerator:
    """Callable class for use as a no-effect command generator.

    The ``__call__`` method for this class simply returns the thing
    you instantiated it with. Example usage::

      env["DO_NOTHING"] = NullCmdGenerator
      env["LINKCOM"] = "${DO_NOTHING('$LINK $SOURCES $TARGET')}"
    """

    def __init__(self, cmd) -> None:
        self.cmd = cmd

    def __call__(self, target, source, env, for_signature=None):
        return self.cmd


class Variable_Method_Caller:
    """A class for finding a construction variable on the stack and
    calling one of its methods.

    Used to support "construction variables" appearing in string
    ``eval``s that actually stand in for methods--specifically, the use
    of "RDirs" in a call to :func:`_concat` that should actually execute the
    ``TARGET.RDirs`` method.

    Historical note: This was formerly supported by creating a little
    "build dictionary" that mapped RDirs to the method, but this got
    in the way of Memoizing construction environments, because we had to
    create new environment objects to hold the variables.
    """

    def __init__(self, variable, method) -> None:
        self.variable = variable
        self.method = method

    def __call__(self, *args, **kw):
        try:
            1 // 0
        except ZeroDivisionError:
            # Don't start iterating with the current stack-frame to
            # prevent creating reference cycles (f_back is safe).
            frame = sys.exc_info()[2].tb_frame.f_back
        variable = self.variable
        while frame:
            if variable in frame.f_locals:
                v = frame.f_locals[variable]
                if v:
                    method = getattr(v, self.method)
                    return method(*args, **kw)
            frame = frame.f_back
        return None


def __libversionflags(env, version_var, flags_var):
    """
    if version_var is not empty, returns env[flags_var], otherwise returns None
    :param env:
    :param version_var:
    :param flags_var:
    :return:
    """
    try:
        if env.subst('$' + version_var):
            return env[flags_var]
    except KeyError:
        pass
    return None


def __lib_either_version_flag(env, version_var1, version_var2, flags_var):
    """
    if $version_var1 or $version_var2 is not empty, returns env[flags_var], otherwise returns None
    :param env:
    :param version_var1:
    :param version_var2:
    :param flags_var:
    :return:
    """
    try:
        if env.subst('$' + version_var1) or env.subst('$' + version_var2):
            return env[flags_var]
    except KeyError:
        pass
    return None





ConstructionEnvironment = {
    'BUILDERS': {},
    'SCANNERS': [SCons.Tool.SourceFileScanner],
    'CONFIGUREDIR': '#/.sconf_temp',
    'CONFIGURELOG': '#/config.log',
    'CPPSUFFIXES': SCons.Tool.CSuffixes,
    'DSUFFIXES': SCons.Tool.DSuffixes,
    'ENV': {},
    'IDLSUFFIXES': SCons.Tool.IDLSuffixes,
    '_concat': _concat,
    '_defines': _defines,
    '_stripixes': _stripixes,
    '_LIBFLAGS': '${_concat(LIBLINKPREFIX, LIBS, LIBLINKSUFFIX, __env__)}',

    '_LIBDIRFLAGS': '${_concat(LIBDIRPREFIX, LIBPATH, LIBDIRSUFFIX, __env__, RDirs, TARGET, SOURCE, affect_signature=False)}',
    '_CPPINCFLAGS': '${_concat(INCPREFIX, CPPPATH, INCSUFFIX, __env__, RDirs, TARGET, SOURCE, affect_signature=False)}',

    '_CPPDEFFLAGS': '${_defines(CPPDEFPREFIX, CPPDEFINES, CPPDEFSUFFIX, __env__, TARGET, SOURCE)}',

    '__libversionflags': __libversionflags,
    '__SHLIBVERSIONFLAGS': '${__libversionflags(__env__,"SHLIBVERSION","_SHLIBVERSIONFLAGS")}',
    '__LDMODULEVERSIONFLAGS': '${__libversionflags(__env__,"LDMODULEVERSION","_LDMODULEVERSIONFLAGS")}',
    '__DSHLIBVERSIONFLAGS': '${__libversionflags(__env__,"DSHLIBVERSION","_DSHLIBVERSIONFLAGS")}',
    '__lib_either_version_flag': __lib_either_version_flag,

    'TEMPFILE': NullCmdGenerator,
    'TEMPFILEARGJOIN': ' ',
    'TEMPFILEARGESCFUNC': SCons.Subst.quote_spaces,
    'Dir': Variable_Method_Caller('TARGET', 'Dir'),
    'Dirs': Variable_Method_Caller('TARGET', 'Dirs'),
    'File': Variable_Method_Caller('TARGET', 'File'),
    'RDirs': Variable_Method_Caller('TARGET', 'RDirs'),
}

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
