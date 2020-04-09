# -*- mode: python; -*-

import os
import shutil
import stat



def _copy(src, dst):
    shutil.copy2(src, dst)
    st = os.stat(src)
    os.chmod(dst, stat.S_IMODE(st[stat.ST_MODE]) | stat.S_IWRITE)

def _symlink(src, dst):
    os.symlink(os.path.relpath(src, os.path.dirname(dst)), dst)

def _hardlink(src, dst):
    try:
        os.link(src, dst)
    except:
        _copy(src, dst)

available_actions = {
    "copy" : _copy,
    "hardlink" : _hardlink,
    "symlink" : _symlink,
}

class _CopytreeError(EnvironmentError):
    pass

def _generate_install_actions(base_action):

    # This is a patched version of shutil.copytree from python 2.5.  It
    # doesn't fail if the dir exists, which regular copytree does
    # (annoyingly).  Note the XXX comment in the docstring.
    def _mongo_copytree(src, dst, symlinks=False):
        """Recursively copy a directory tree using copy2().

        The destination directory must not already exist.
        If exception(s) occur, an _CopytreeError is raised with a list of reasons.

        If the optional symlinks flag is true, symbolic links in the
        source tree result in symbolic links in the destination tree; if
        it is false, the contents of the files pointed to by symbolic
        links are copied.

        XXX Consider this example code rather than the ultimate tool.

        """
        names = os.listdir(src)
        # garyo@genarts.com fix: check for dir before making dirs.
        if not os.path.exists(dst):
            os.makedirs(dst)
        errors = []
        for name in names:
            srcname = os.path.join(src, name)
            dstname = os.path.join(dst, name)
            try:
                if symlinks and os.path.islink(srcname):
                    linkto = os.readlink(srcname)
                    os.symlink(linkto, dstname)
                elif os.path.isdir(srcname):
                    _mongo_copytree(srcname, dstname, symlinks)
                else:
                    base_action(srcname, dstname)
                # XXX What about devices, sockets etc.?
            except (IOError, os.error) as why:
                errors.append((srcname, dstname, str(why)))
            # catch the _CopytreeError from the recursive copytree so that we can
            # continue with other files
            except _CopytreeError as err:
                errors.extend(err.args[0])
        try:
            shutil.copystat(src, dst)
        except SCons.Util.WinError:
            # can't copy file access times on Windows
            pass
        except OSError as why:
            errors.extend((src, dst, str(why)))
        if errors:
            raise _CopytreeError(errors)


    #
    # Functions doing the actual work of the Install Builder.
    #
    def _mongo_copyFunc(dest, source, env):
        """Install a source file or directory into a destination by copying,
        (including copying permission/mode bits)."""

        if os.path.isdir(source):
            if os.path.exists(dest):
                if not os.path.isdir(dest):
                    raise SCons.Errors.UserError("cannot overwrite non-directory `%s' with a directory `%s'" % (str(dest), str(source)))
            else:
                parent = os.path.split(dest)[0]
                if not os.path.exists(parent):
                    os.makedirs(parent)
            _mongo_copytree(source, dest)
        else:
            base_action(source, dest)

        return 0

    #
    # Functions doing the actual work of the InstallVersionedLib Builder.
    #
    def _mongo_copyFuncVersionedLib(dest, source, env):
        """Install a versioned library into a destination by copying,
        (including copying permission/mode bits) and then creating
        required symlinks."""

        if os.path.isdir(source):
            raise SCons.Errors.UserError("cannot install directory `%s' as a version library" % str(source) )
        else:
            # remove the link if it is already there
            try:
                os.remove(dest)
            except:
                pass
            base_action(source, dest)
            SCons.tool.install.installShlibLinks(dest, source, env)

        return 0

    return (_mongo_copyFunc, _mongo_copyFuncVersionedLib)


def setup(env, action):
    if action == "default":
        return
    base_action = available_actions.get(action, None)
    handlers = _generate_install_actions(base_action)
    env['INSTALL'] = handlers[0]
    env['INSTALLVERSIONEDLIB'] = handlers[1]
