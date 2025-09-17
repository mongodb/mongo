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

""" Tool-specific initialization for the install tool.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.
"""

import os
import stat
from shutil import copy2, copystat

import SCons.Action
import SCons.Tool
import SCons.Util
from SCons.Subst import SUBST_RAW
from SCons.Tool.linkCommon import (
    StringizeLibSymlinks,
    CreateLibSymlinks,
    EmitLibSymlinks,
)

# We keep track of *all* installed files.
_INSTALLED_FILES = []
_UNIQUE_INSTALLED_FILES = None

class CopytreeError(OSError):
    pass


def scons_copytree(src, dst, symlinks: bool=False, ignore=None, copy_function=copy2,
                   ignore_dangling_symlinks: bool=False, dirs_exist_ok: bool=False):
    """Recursively copy a directory tree, SCons version.

    This is a modified copy of the Python 3.7 shutil.copytree function.
    SCons update: dirs_exist_ok dictates whether to raise an
    exception in case dst or any missing parent directory already
    exists. Implementation depends on os.makedirs having a similar
    flag, which it has since Python 3.2.  This version also raises an
    SCons-defined exception rather than the one defined locally to shtuil.
    This version uses a change from Python 3.8.
    TODO: we can remove this forked copy once the minimum Py version is 3.8.

    If exception(s) occur, an Error is raised with a list of reasons.

    If the optional symlinks flag is true, symbolic links in the
    source tree result in symbolic links in the destination tree; if
    it is false, the contents of the files pointed to by symbolic
    links are copied. If the file pointed by the symlink doesn't
    exist, an exception will be added in the list of errors raised in
    an Error exception at the end of the copy process.

    You can set the optional ignore_dangling_symlinks flag to true if you
    want to silence this exception. Notice that this has no effect on
    platforms that don't support os.symlink.

    The optional ignore argument is a callable. If given, it
    is called with the `src` parameter, which is the directory
    being visited by copytree(), and `names` which is the list of
    `src` contents, as returned by os.listdir():

        callable(src, names) -> ignored_names

    Since copytree() is called recursively, the callable will be
    called once for each directory that is copied. It returns a
    list of names relative to the `src` directory that should
    not be copied.

    The optional copy_function argument is a callable that will be used
    to copy each file. It will be called with the source path and the
    destination path as arguments. By default, copy2() is used, but any
    function that supports the same signature (like copy()) can be used.

    """
    names = os.listdir(src)
    if ignore is not None:
        ignored_names = ignore(src, names)
    else:
        ignored_names = set()

    os.makedirs(dst, exist_ok=dirs_exist_ok)
    errors = []
    for name in names:
        if name in ignored_names:
            continue
        srcname = os.path.join(src, name)
        dstname = os.path.join(dst, name)
        try:
            if os.path.islink(srcname):
                linkto = os.readlink(srcname)
                if symlinks:
                    # We can't just leave it to `copy_function` because legacy
                    # code with a custom `copy_function` may rely on copytree
                    # doing the right thing.
                    os.symlink(linkto, dstname)
                    copystat(srcname, dstname, follow_symlinks=not symlinks)
                else:
                    # ignore dangling symlink if the flag is on
                    if not os.path.exists(linkto) and ignore_dangling_symlinks:
                        continue
                    # otherwise let the copy occurs. copy2 will raise an error
                    if os.path.isdir(srcname):
                        scons_copytree(srcname, dstname, symlinks=symlinks,
                                       ignore=ignore, copy_function=copy_function,
                                       ignore_dangling_symlinks=ignore_dangling_symlinks,
                                       dirs_exist_ok=dirs_exist_ok)
                    else:
                        copy_function(srcname, dstname)
            elif os.path.isdir(srcname):
                scons_copytree(srcname, dstname, symlinks=symlinks,
                               ignore=ignore, copy_function=copy_function,
                               ignore_dangling_symlinks=ignore_dangling_symlinks,
                               dirs_exist_ok=dirs_exist_ok)
            else:
                # Will raise a SpecialFileError for unsupported file types
                copy_function(srcname, dstname)
        # catch the Error from the recursive copytree so that we can
        # continue with other files
        except CopytreeError as err:  # SCons change
            errors.extend(err.args[0])
        except OSError as why:
            errors.append((srcname, dstname, str(why)))
    try:
        copystat(src, dst)
    except OSError as why:
        # Copying file access times may fail on Windows
        if getattr(why, 'winerror', None) is None:
            errors.append((src, dst, str(why)))
    if errors:
        raise CopytreeError(errors)  # SCons change
    return dst

#
# Functions doing the actual work of the Install Builder.
#
def copyFunc(dest, source, env) -> int:
    """Install a source file or directory into a destination by copying.

    Mode/permissions bits will be copied as well, except that the target
    will be made writable.

    Returns:
        POSIX-style error code - 0 for success, non-zero for fail
    """
    if os.path.isdir(source):
        if os.path.exists(dest):
            if not os.path.isdir(dest):
                raise SCons.Errors.UserError("cannot overwrite non-directory `%s' with a directory `%s'" % (str(dest), str(source)))
        else:
            parent = os.path.split(dest)[0]
            if not os.path.exists(parent):
                os.makedirs(parent)
        scons_copytree(source, dest, dirs_exist_ok=True)
    else:
        copy2(source, dest)
        st = os.stat(source)
        os.chmod(dest, stat.S_IMODE(st.st_mode) | stat.S_IWRITE)

    return 0

#
# Functions doing the actual work of the InstallVersionedLib Builder.
#
def copyFuncVersionedLib(dest, source, env) -> int:
    """Install a versioned library into a destination by copying.

    Any required symbolic links for other library names are created.

    Mode/permissions bits will be copied as well, except that the target
    will be made writable.

    Returns:
        POSIX-style error code - 0 for success, non-zero for fail
    """
    if os.path.isdir(source):
        raise SCons.Errors.UserError("cannot install directory `%s' as a version library" % str(source) )
    else:
        # remove the link if it is already there
        try:
            os.remove(dest)
        except:
            pass
        copy2(source, dest)
        st = os.stat(source)
        os.chmod(dest, stat.S_IMODE(st.st_mode) | stat.S_IWRITE)
        installShlibLinks(dest, source, env)

    return 0

def listShlibLinksToInstall(dest, source, env):
    install_links = []
    source = env.arg2nodes(source)
    dest = env.fs.File(dest)
    install_dir = dest.get_dir()
    for src in source:
        symlinks = getattr(getattr(src, 'attributes', None), 'shliblinks', None)
        if symlinks:
            for link, linktgt in symlinks:
                link_base = os.path.basename(link.get_path())
                linktgt_base = os.path.basename(linktgt.get_path())
                install_link = env.fs.File(link_base, install_dir)
                install_linktgt = env.fs.File(linktgt_base, install_dir)
                install_links.append((install_link, install_linktgt))
    return install_links

def installShlibLinks(dest, source, env) -> None:
    """If we are installing a versioned shared library create the required links."""
    Verbose = False
    symlinks = listShlibLinksToInstall(dest, source, env)
    if Verbose:
        print(f'installShlibLinks: symlinks={StringizeLibSymlinks(symlinks)!r}')
    if symlinks:
        CreateLibSymlinks(env, symlinks)
    return

def installFunc(target, source, env) -> int:
    """Install a source file into a target.

    Uses the function specified in the INSTALL construction variable.

    Returns:
        POSIX-style error code - 0 for success, non-zero for fail
    """

    try:
        install = env['INSTALL']
    except KeyError:
        raise SCons.Errors.UserError('Missing INSTALL construction variable.')

    assert len(target) == len(source), (
        "Installing source %s into target %s: "
        "target and source lists must have same length."
        % (list(map(str, source)), list(map(str, target)))
    )
    for t, s in zip(target, source):
        if install(t.get_path(), s.get_path(), env):
            return 1

    return 0

def installFuncVersionedLib(target, source, env) -> int:
    """Install a versioned library into a target.

    Uses the function specified in the INSTALL construction variable.

    Returns:
        POSIX-style error code - 0 for success, non-zero for fail
    """

    try:
        install = env['INSTALLVERSIONEDLIB']
    except KeyError:
        raise SCons.Errors.UserError(
            'Missing INSTALLVERSIONEDLIB construction variable.'
        )

    assert len(target) == len(source), (
        "Installing source %s into target %s: "
        "target and source lists must have same length."
        % (list(map(str, source)), list(map(str, target)))
    )
    for t, s in zip(target, source):
        if hasattr(t.attributes, 'shlibname'):
            tpath = os.path.join(t.get_dir(), t.attributes.shlibname)
        else:
            tpath = t.get_path()
        if install(tpath, s.get_path(), env):
            return 1

    return 0

def stringFunc(target, source, env):
    installstr = env.get('INSTALLSTR')
    if installstr:
        return env.subst_target_source(installstr, SUBST_RAW, target, source)
    target = str(target[0])
    source = str(source[0])
    if os.path.isdir(source):
        type = 'directory'
    else:
        type = 'file'
    return 'Install %s: "%s" as "%s"' % (type, source, target)

#
# Emitter functions
#
def add_targets_to_INSTALLED_FILES(target, source, env):
    """ An emitter that adds all target files to the list stored in the
    _INSTALLED_FILES global variable. This way all installed files of one
    scons call will be collected.
    """
    global _INSTALLED_FILES, _UNIQUE_INSTALLED_FILES
    _INSTALLED_FILES.extend(target)

    _UNIQUE_INSTALLED_FILES = None
    return (target, source)

def add_versioned_targets_to_INSTALLED_FILES(target, source, env):
    """ An emitter that adds all target files to the list stored in the
    _INSTALLED_FILES global variable. This way all installed files of one
    scons call will be collected.
    """
    global _INSTALLED_FILES, _UNIQUE_INSTALLED_FILES
    Verbose = False
    _INSTALLED_FILES.extend(target)
    if Verbose:
        print(f"add_versioned_targets_to_INSTALLED_FILES: target={list(map(str, target))!r}")
    symlinks = listShlibLinksToInstall(target[0], source, env)
    if symlinks:
        EmitLibSymlinks(env, symlinks, target[0])
    _UNIQUE_INSTALLED_FILES = None
    return (target, source)

class DESTDIR_factory:
    """ A node factory, where all files will be relative to the dir supplied
    in the constructor.
    """
    def __init__(self, env, dir) -> None:
        self.env = env
        self.dir = env.arg2nodes( dir, env.fs.Dir )[0]

    def Entry(self, name):
        name = SCons.Util.make_path_relative(name)
        return self.dir.Entry(name)

    def Dir(self, name):
        name = SCons.Util.make_path_relative(name)
        return self.dir.Dir(name)

#
# The Builder Definition
#
install_action       = SCons.Action.Action(installFunc, stringFunc)
installas_action     = SCons.Action.Action(installFunc, stringFunc)
installVerLib_action = SCons.Action.Action(installFuncVersionedLib, stringFunc)

BaseInstallBuilder               = None

def InstallBuilderWrapper(env, target=None, source=None, dir=None, **kw):
    if target and dir:
        import SCons.Errors
        raise SCons.Errors.UserError("Both target and dir defined for Install(), only one may be defined.")
    if not dir:
        dir=target

    import SCons.Script
    install_sandbox = SCons.Script.GetOption('install_sandbox')
    if install_sandbox:
        target_factory = DESTDIR_factory(env, install_sandbox)
    else:
        target_factory = env.fs

    try:
        dnodes = env.arg2nodes(dir, target_factory.Dir)
    except TypeError:
        raise SCons.Errors.UserError("Target `%s' of Install() is a file, but should be a directory.  Perhaps you have the Install() arguments backwards?" % str(dir))
    sources = env.arg2nodes(source, env.fs.Entry)
    tgt = []
    for dnode in dnodes:
        for src in sources:
            # Prepend './' so the lookup doesn't interpret an initial
            # '#' on the file name portion as meaning the Node should
            # be relative to the top-level SConstruct directory.
            target = env.fs.Entry('.'+os.sep+src.name, dnode)
            tgt.extend(BaseInstallBuilder(env, target, src, **kw))
    return tgt


def InstallAsBuilderWrapper(env, target=None, source=None, **kw):
    result = []
    for src, tgt in map(lambda x, y: (x, y), source, target):
        result.extend(BaseInstallBuilder(env, tgt, src, **kw))
    return result

BaseVersionedInstallBuilder = None


def InstallVersionedBuilderWrapper(env, target=None, source=None, dir=None, **kw):
    if target and dir:
        import SCons.Errors
        raise SCons.Errors.UserError("Both target and dir defined for Install(), only one may be defined.")
    if not dir:
        dir=target

    import SCons.Script
    install_sandbox = SCons.Script.GetOption('install_sandbox')
    if install_sandbox:
        target_factory = DESTDIR_factory(env, install_sandbox)
    else:
        target_factory = env.fs

    try:
        dnodes = env.arg2nodes(dir, target_factory.Dir)
    except TypeError:
        raise SCons.Errors.UserError("Target `%s' of Install() is a file, but should be a directory.  Perhaps you have the Install() arguments backwards?" % str(dir))
    sources = env.arg2nodes(source, env.fs.Entry)
    tgt = []
    for dnode in dnodes:
        for src in sources:
            # Prepend './' so the lookup doesn't interpret an initial
            # '#' on the file name portion as meaning the Node should
            # be relative to the top-level SConstruct directory.
            target = env.fs.Entry('.'+os.sep+src.name, dnode)
            tgt.extend(BaseVersionedInstallBuilder(env, target, src, **kw))
    return tgt

added = None


def generate(env) -> None:

    from SCons.Script import AddOption, GetOption
    global added
    if not added:
        added = 1
        AddOption('--install-sandbox',
                  dest='install_sandbox',
                  type="string",
                  action="store",
                  help='A directory under which all installed files will be placed.')

    global BaseInstallBuilder
    if BaseInstallBuilder is None:
        install_sandbox = GetOption('install_sandbox')
        if install_sandbox:
            target_factory = DESTDIR_factory(env, install_sandbox)
        else:
            target_factory = env.fs

        BaseInstallBuilder = SCons.Builder.Builder(
                              action         = install_action,
                              target_factory = target_factory.Entry,
                              source_factory = env.fs.Entry,
                              multi          = True,
                              emitter        = [ add_targets_to_INSTALLED_FILES, ],
                              source_scanner = SCons.Scanner.ScannerBase({}, name='Install', recursive=False),
                              name           = 'InstallBuilder')

    global BaseVersionedInstallBuilder
    if BaseVersionedInstallBuilder is None:
        install_sandbox = GetOption('install_sandbox')
        if install_sandbox:
            target_factory = DESTDIR_factory(env, install_sandbox)
        else:
            target_factory = env.fs

        BaseVersionedInstallBuilder = SCons.Builder.Builder(
                                       action         = installVerLib_action,
                                       target_factory = target_factory.Entry,
                                       source_factory = env.fs.Entry,
                                       multi          = True,
                                       emitter        = [ add_versioned_targets_to_INSTALLED_FILES, ],
                                       name           = 'InstallVersionedBuilder')

    env['BUILDERS']['_InternalInstall'] = InstallBuilderWrapper
    env['BUILDERS']['_InternalInstallAs'] = InstallAsBuilderWrapper
    env['BUILDERS']['_InternalInstallVersionedLib'] = InstallVersionedBuilderWrapper

    # We'd like to initialize this doing something like the following,
    # but there isn't yet support for a ${SOURCE.type} expansion that
    # will print "file" or "directory" depending on what's being
    # installed.  For now we punt by not initializing it, and letting
    # the stringFunc() that we put in the action fall back to the
    # hand-crafted default string if it's not set.
    #
    #try:
    #    env['INSTALLSTR']
    #except KeyError:
    #    env['INSTALLSTR'] = 'Install ${SOURCE.type}: "$SOURCES" as "$TARGETS"'

    try:
        env['INSTALL']
    except KeyError:
        env['INSTALL'] = copyFunc

    try:
        env['INSTALLVERSIONEDLIB']
    except KeyError:
        env['INSTALLVERSIONEDLIB'] = copyFuncVersionedLib

def exists(env) -> bool:
    return True

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
