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

"""SCons platform selection.

Looks for modules that define a callable object that can modify a
construction environment as appropriate for a given platform.

Note that we take a more simplistic view of "platform" than Python does.
We're looking for a single string that determines a set of
tool-independent variables with which to initialize a construction
environment.  Consequently, we'll examine both sys.platform and os.name
(and anything else that might come in to play) in order to return some
specification which is unique enough for our purposes.

Note that because this subsystem just *selects* a callable that can
modify a construction environment, it's possible for people to define
their own "platform specification" in an arbitrary callable function.
No one needs to use or tie in to this subsystem in order to roll
their own platform definition.
"""

import SCons.compat

import atexit
import importlib
import os
import sys
import tempfile

import SCons.Errors
import SCons.Subst
import SCons.Tool


def platform_default():
    r"""Return the platform string for our execution environment.

    The returned value should map to one of the SCons/Platform/\*.py
    files.  Since scons is architecture independent, though, we don't
    care about the machine architecture.
    """
    osname = os.name
    if osname == 'java':
        osname = os._osType
    if osname == 'posix':
        if sys.platform == 'cygwin':
            return 'cygwin'
        elif sys.platform.find('irix') != -1:
            return 'irix'
        elif sys.platform.find('sunos') != -1:
            return 'sunos'
        elif sys.platform.find('hp-ux') != -1:
            return 'hpux'
        elif sys.platform.find('aix') != -1:
            return 'aix'
        elif sys.platform.find('darwin') != -1:
            return 'darwin'
        else:
            return 'posix'
    elif os.name == 'os2':
        return 'os2'
    else:
        return sys.platform


def platform_module(name=platform_default()):
    """Return the imported module for the platform.

    This looks for a module name that matches the specified argument.
    If the name is unspecified, we fetch the appropriate default for
    our execution environment.
    """
    full_name = 'SCons.Platform.' + name
    try:
        return sys.modules[full_name]
    except KeyError:
        try:
            # the specific platform module is a relative import
            mod = importlib.import_module("." + name, __name__)
        except ModuleNotFoundError:
            try:
                # This support was added to enable running inside
                # a py2exe bundle a long time ago - unclear if it's
                # still needed. It is *not* intended to load individual
                # platform modules stored in a zipfile.
                import zipimport

                platform = sys.modules['SCons.Platform'].__path__[0]
                importer = zipimport.zipimporter(platform)
                if not hasattr(importer, 'find_spec'):
                    # zipimport only added find_spec, exec_module in 3.10,
                    # unlike importlib, where they've been around since 3.4.
                    # If we don't have 'em, use the old way.
                    mod = importer.load_module(full_name)
                else:
                    spec = importer.find_spec(full_name)
                    mod = importlib.util.module_from_spec(spec)
                    importer.exec_module(mod)
                sys.modules[full_name] = mod
            except zipimport.ZipImportError:
                raise SCons.Errors.UserError("No platform named '%s'" % name)

        setattr(SCons.Platform, name, mod)
        return mod


def DefaultToolList(platform, env):
    """Select a default tool list for the specified platform."""
    return SCons.Tool.tool_list(platform, env)


class PlatformSpec:
    def __init__(self, name, generate) -> None:
        self.name = name
        self.generate = generate

    def __call__(self, *args, **kw):
        return self.generate(*args, **kw)

    def __str__(self) -> str:
        return self.name


class TempFileMunge:
    """Convert long command lines to use a temporary file.

    You can set an Environment variable (usually ``TEMPFILE``) to this,
    then call it with a string argument, and it will perform temporary
    file substitution on it.  This is used to circumvent limitations on
    the length of command lines. Example::

        env["TEMPFILE"] = TempFileMunge
        env["LINKCOM"] = "${TEMPFILE('$LINK $TARGET $SOURCES', '$LINKCOMSTR')}"

    By default, the name of the temporary file used begins with a
    prefix of '@'.  This may be configured for other tool chains by
    setting the ``TEMPFILEPREFIX`` variable. Example::

        env["TEMPFILEPREFIX"] = '-@'        # diab compiler
        env["TEMPFILEPREFIX"] = '-via'      # arm tool chain
        env["TEMPFILEPREFIX"] = ''          # (the empty string) PC Lint

    You can configure the extension of the temporary file through the
    ``TEMPFILESUFFIX`` variable, which defaults to '.lnk' (see comments
    in the code below). Example::

        env["TEMPFILESUFFIX"] = '.lnt'   # PC Lint

    Entries in the temporary file are separated by the value of the
    ``TEMPFILEARGJOIN`` variable, which defaults to an OS-appropriate value.

    A default argument escape function is ``SCons.Subst.quote_spaces``.
    If you need to apply extra operations on a command argument before
    writing to a temporary file(fix Windows slashes, normalize paths, etc.),
    please set `TEMPFILEARGESCFUNC` variable to a custom function. Example::

        import sys
        import re
        from SCons.Subst import quote_spaces

        WINPATHSEP_RE = re.compile(r"\\([^\"'\\]|$)")


        def tempfile_arg_esc_func(arg):
            arg = quote_spaces(arg)
            if sys.platform != "win32":
                return arg
            # GCC requires double Windows slashes, let's use UNIX separator
            return WINPATHSEP_RE.sub(r"/\1", arg)


        env["TEMPFILEARGESCFUNC"] = tempfile_arg_esc_func

    """
    def __init__(self, cmd, cmdstr = None) -> None:
        self.cmd = cmd
        self.cmdstr = cmdstr

    def __call__(self, target, source, env, for_signature):
        if for_signature:
            # If we're being called for signature calculation, it's
            # because we're being called by the string expansion in
            # Subst.py, which has the logic to strip any $( $) that
            # may be in the command line we squirreled away.  So we
            # just return the raw command line and let the upper
            # string substitution layers do their thing.
            return self.cmd

        # Now we're actually being called because someone is actually
        # going to try to execute the command, so we have to do our
        # own expansion.
        cmd = env.subst_list(self.cmd, SCons.Subst.SUBST_CMD, target, source)[0]
        try:
            maxline = int(env.subst('$MAXLINELENGTH'))
        except ValueError:
            maxline = 2048

        length = 0
        for c in cmd:
            length += len(c)
        length += len(cmd) - 1
        if length <= maxline:
            return self.cmd

        # Check if we already created the temporary file for this target
        # It should have been previously done by Action.strfunction() call
        if SCons.Util.is_List(target):
            node = target[0]
        else:
            node = target

        cmdlist = None

        if SCons.Util.is_List(self.cmd):
            cmdlist_key = tuple(self.cmd)
        else:
            cmdlist_key = self.cmd

        if node and hasattr(node.attributes, 'tempfile_cmdlist'):
            cmdlist = node.attributes.tempfile_cmdlist.get(cmdlist_key, None)
        if cmdlist is not None:
            return cmdlist

        # Default to the .lnk suffix for the benefit of the Phar Lap
        # linkloc linker, which likes to append an .lnk suffix if
        # none is given.
        if 'TEMPFILESUFFIX' in env:
            suffix = env.subst('$TEMPFILESUFFIX')
        else:
            suffix = '.lnk'

        if 'TEMPFILEDIR' in env:
            tempfile_dir = env.subst('$TEMPFILEDIR')
            os.makedirs(tempfile_dir, exist_ok=True)
        else:
            tempfile_dir = None

        fd, tmp = tempfile.mkstemp(suffix, dir=tempfile_dir, text=True)
        native_tmp = SCons.Util.get_native_path(tmp)

        # arrange for cleanup on exit:

        def tmpfile_cleanup(file) -> None:
            os.remove(file)

        atexit.register(tmpfile_cleanup, tmp)

        if env.get('SHELL', None) == 'sh':
            # The sh shell will try to escape the backslashes in the
            # path, so unescape them.
            native_tmp = native_tmp.replace('\\', r'\\\\')

        if 'TEMPFILEPREFIX' in env:
            prefix = env.subst('$TEMPFILEPREFIX')
        else:
            prefix = "@"

        tempfile_esc_func = env.get('TEMPFILEARGESCFUNC', SCons.Subst.quote_spaces)
        args = [tempfile_esc_func(arg) for arg in cmd[1:]]
        join_char = env.get('TEMPFILEARGJOIN', ' ')
        os.write(fd, bytearray(join_char.join(args) + "\n", encoding="utf-8"))
        os.close(fd)

        # XXX Using the SCons.Action.print_actions value directly
        # like this is bogus, but expedient.  This class should
        # really be rewritten as an Action that defines the
        # __call__() and strfunction() methods and lets the
        # normal action-execution logic handle whether or not to
        # print/execute the action.  The problem, though, is all
        # of that is decided before we execute this method as
        # part of expanding the $TEMPFILE construction variable.
        # Consequently, refactoring this will have to wait until
        # we get more flexible with allowing Actions to exist
        # independently and get strung together arbitrarily like
        # Ant tasks.  In the meantime, it's going to be more
        # user-friendly to not let obsession with architectural
        # purity get in the way of just being helpful, so we'll
        # reach into SCons.Action directly.
        if SCons.Action.print_actions:
            cmdstr = (
                env.subst(self.cmdstr, SCons.Subst.SUBST_RAW, target, source)
                if self.cmdstr is not None
                else ''
            )
            # Print our message only if XXXCOMSTR returns an empty string
            if not cmdstr:
                cmdstr = (
                    f"Using tempfile {native_tmp} for command line:\n"
                    f'{cmd[0]} {" ".join(args)}'
                )
                self._print_cmd_str(target, source, env, cmdstr)

        cmdlist = [cmd[0], prefix + native_tmp]

        # Store the temporary file command list into the target Node.attributes
        # to avoid creating two temporary files one for print and one for execute.
        if node is not None:
            try:
                # Storing in tempfile_cmdlist by self.cmd provided when intializing
                # $TEMPFILE{} fixes issue raised in PR #3140 and #3553
                node.attributes.tempfile_cmdlist[cmdlist_key] = cmdlist
            except AttributeError:
                node.attributes.tempfile_cmdlist = {cmdlist_key: cmdlist}

        return cmdlist

    def _print_cmd_str(self, target, source, env, cmdstr) -> None:
        # check if the user has specified a cmd line print function
        print_func = None
        try:
            get = env.get
        except AttributeError:
            pass
        else:
            print_func = get('PRINT_CMD_LINE_FUNC')

        # use the default action cmd line print if user did not supply one
        if not print_func:
            action = SCons.Action._ActionAction()
            action.print_cmd_line(cmdstr, target, source, env)
        else:
            print_func(cmdstr, target, source, env)


def Platform(name = platform_default()):
    """Select a canned Platform specification."""

    module = platform_module(name)
    spec = PlatformSpec(name, module.generate)
    return spec

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
