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
Common link/shared library logic
"""

import SCons.Util
import SCons.Warnings
from SCons.Tool.DCommon import isD
from SCons.Util import is_List

issued_mixed_link_warning = False


def StringizeLibSymlinks(symlinks):
    """Converts list with pairs of nodes to list with pairs of node paths
    (strings). Used mainly for debugging."""
    if is_List(symlinks):
        try:
            return [(k.get_path(), v.get_path()) for k, v in symlinks]
        except (TypeError, ValueError):
            return symlinks
    else:
        return symlinks


def EmitLibSymlinks(env, symlinks, libnode, **kw) -> None:
    """Used by emitters to handle (shared/versioned) library symlinks"""
    Verbose = False

    # nodes involved in process... all symlinks + library
    nodes = list(set([x for x, y in symlinks] + [libnode]))

    clean_targets = kw.get('clean_targets', [])
    if not is_List(clean_targets):
        clean_targets = [clean_targets]

    for link, linktgt in symlinks:
        env.SideEffect(link, linktgt)
        if Verbose:
            print("EmitLibSymlinks: SideEffect(%r,%r)" % (link.get_path(), linktgt.get_path()))
        clean_list = [x for x in nodes if x != linktgt]
        env.Clean(list(set([linktgt] + clean_targets)), clean_list)
        if Verbose:
            print("EmitLibSymlinks: Clean(%r,%r)" % (linktgt.get_path(), [x.get_path() for x in clean_list]))


def CreateLibSymlinks(env, symlinks) -> int:
    """Physically creates symlinks. The symlinks argument must be a list in
    form [ (link, linktarget), ... ], where link and linktarget are SCons
    nodes.
    """
    Verbose = False

    for link, linktgt in symlinks:
        linktgt = link.get_dir().rel_path(linktgt)
        link = link.get_path()
        if Verbose:
            print("CreateLibSymlinks: preparing to add symlink %r -> %r" % (link, linktgt))
        # Delete the (previously created) symlink if exists. Let only symlinks
        # to be deleted to prevent accidental deletion of source files...
        if env.fs.islink(link):
            env.fs.unlink(link)
            if Verbose:
                print("CreateLibSymlinks: removed old symlink %r" % link)
        # If a file or directory exists with the same name as link, an OSError
        # will be thrown, which should be enough, I think.
        env.fs.symlink(linktgt, link)
        if Verbose:
            print("CreateLibSymlinks: add symlink %r -> %r" % (link, linktgt))
    return 0


def LibSymlinksActionFunction(target, source, env) -> int:
    for tgt in target:
        symlinks = getattr(getattr(tgt, 'attributes', None), 'shliblinks', None)
        if symlinks:
            CreateLibSymlinks(env, symlinks)
    return 0


def LibSymlinksStrFun(target, source, env, *args):
    cmd = None
    for tgt in target:
        symlinks = getattr(getattr(tgt, 'attributes', None), 'shliblinks', None)
        if symlinks:
            if cmd is None: cmd = ""
            if cmd: cmd += "\n"
            cmd += "Create symlinks for: %r\n    " % tgt.get_path()
            try:
                linkstr = '\n    '.join(["%r->%r" % (k, v) for k, v in StringizeLibSymlinks(symlinks)])
            except (KeyError, ValueError):
                pass
            else:
                cmd += "%s" % linkstr
    return cmd


def _call_env_subst(env, string, *args, **kw):
    kw2 = {}
    for k in ('raw', 'target', 'source', 'conv', 'executor'):
        try:
            kw2[k] = kw[k]
        except KeyError:
            pass
    return env.subst(string, *args, **kw2)


def smart_link(source, target, env, for_signature) -> str:
    import SCons.Tool.cxx
    import SCons.Tool.FortranCommon

    has_cplusplus = SCons.Tool.cxx.iscplusplus(source)
    has_fortran = SCons.Tool.FortranCommon.isfortran(env, source)
    has_d = isD(env, source)
    if has_cplusplus and has_fortran and not has_d:
        global issued_mixed_link_warning
        if not issued_mixed_link_warning:
            msg = (
                "Using $CXX to link Fortran and C++ code together.\n"
                "    This may generate a buggy executable if the '%s'\n"
                "    compiler does not know how to deal with Fortran runtimes."
            )
            SCons.Warnings.warn(
                SCons.Warnings.FortranCxxMixWarning, msg % env.subst('$CXX')
            )
            issued_mixed_link_warning = True
        return '$CXX'
    elif has_d:
        env['LINKCOM'] = env['DLINKCOM']
        env['SHLINKCOM'] = env['SHDLINKCOM']
        return '$DC'
    elif has_fortran:
        return '$FORTRAN'
    elif has_cplusplus:
        return '$CXX'
    return '$CC'


def lib_emitter(target, source, env, **kw):
    verbose = False
    if verbose:
        print(f"_lib_emitter: target[0]={target[0].get_path()!r}")
    for tgt in target:
        if SCons.Util.is_String(tgt):
            tgt = env.File(tgt)
        tgt.attributes.shared = True

    return target, source
