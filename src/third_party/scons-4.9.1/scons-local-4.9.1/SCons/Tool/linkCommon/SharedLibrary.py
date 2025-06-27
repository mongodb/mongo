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

from SCons.Errors import UserError
from SCons.Tool import createSharedLibBuilder
from SCons.Util import CLVar
from . import lib_emitter, EmitLibSymlinks, StringizeLibSymlinks


def shlib_symlink_emitter(target, source, env, **kw):
    verbose = False

    if "variable_prefix" in kw:
        var_prefix = kw["variable_prefix"]
    else:
        var_prefix = "SHLIB"

    do_symlinks = env.subst("$%sNOVERSIONSYMLINKS" % var_prefix)
    if do_symlinks in ["1", "True", "true", True]:
        return target, source

    shlibversion = env.subst("$%sVERSION" % var_prefix)
    if shlibversion:
        if verbose:
            print("shlib_symlink_emitter: %sVERSION=%s" % (var_prefix, shlibversion))

        libnode = target[0]
        shlib_soname_symlink = env.subst(
            "$%s_SONAME_SYMLINK" % var_prefix, target=target, source=source
        )
        shlib_noversion_symlink = env.subst(
            "$%s_NOVERSION_SYMLINK" % var_prefix, target=target, source=source
        )

        if verbose:
            print("shlib_soname_symlink    :%s" % shlib_soname_symlink)
            print("shlib_noversion_symlink :%s" % shlib_noversion_symlink)
            print("libnode                 :%s" % libnode)

        shlib_soname_symlink = env.File(shlib_soname_symlink)
        shlib_noversion_symlink = env.File(shlib_noversion_symlink)

        symlinks = []
        if shlib_soname_symlink != libnode:
            # If soname and library name machine, don't symlink them together
            symlinks.append((env.File(shlib_soname_symlink), libnode))

        symlinks.append((env.File(shlib_noversion_symlink), libnode))

        if verbose:
            print(
                "_lib_emitter: symlinks={!r}".format(
                    ", ".join(
                        ["%r->%r" % (k, v) for k, v in StringizeLibSymlinks(symlinks)]
                    )
                )
            )

        if symlinks:
            # This does the actual symlinking
            EmitLibSymlinks(env, symlinks, target[0])

            # This saves the information so if the versioned shared library is installed
            # it can faithfully reproduce the correct symlinks
            target[0].attributes.shliblinks = symlinks

    return target, source


def _soversion(target, source, env, for_signature):
    """Function to determine what to use for SOVERSION"""

    if "SOVERSION" in env:
        return ".$SOVERSION"
    elif "SHLIBVERSION" in env:
        shlibversion = env.subst("$SHLIBVERSION")
        # We use only the most significant digit of SHLIBVERSION
        return "." + shlibversion.split(".")[0]
    else:
        return ""


def _soname(target, source, env, for_signature) -> str:
    if "SONAME" in env:
        # Now verify that SOVERSION is not also set as that is not allowed
        if "SOVERSION" in env:
            raise UserError(
                "Ambiguous library .so naming, both SONAME: %s and SOVERSION: %s are defined. "
                "Only one can be defined for a target library."
                % (env["SONAME"], env["SOVERSION"])
            )
        return "$SONAME"
    else:
        return "$SHLIBPREFIX$_get_shlib_stem${SHLIBSUFFIX}$_SHLIBSOVERSION"


def _get_shlib_stem(target, source, env, for_signature: bool) -> str:
    """Get the base name of a shared library.

    Args:
        target: target node containing the lib name
        source: source node, not used
        env: environment context for running subst
        for_signature: whether this is being done for signature generation

    Returns:
        the library name without prefix/suffix
    """
    verbose = False

    target_name = str(target.name)
    shlibprefix = env.subst("$SHLIBPREFIX")
    shlibsuffix = env.subst("$_SHLIBSUFFIX")

    if verbose and not for_signature:
        print(
            "_get_shlib_stem: target_name:%s shlibprefix:%s shlibsuffix:%s"
            % (target_name, shlibprefix, shlibsuffix)
        )


    if shlibsuffix and target_name.endswith(shlibsuffix):
        target_name = target_name[: -len(shlibsuffix)]

    if shlibprefix and target_name.startswith(shlibprefix):
        # skip pathological case were target _is_ the prefix
        if target_name != shlibprefix:
            target_name = target_name[len(shlibprefix) :]


    if verbose and not for_signature:
        print("_get_shlib_stem: target_name:%s AFTER" % (target_name,))

    return target_name


def _get_shlib_dir(target, source, env, for_signature: bool) -> str:
    """Get the directory the shared library is in.

    Args:
        target: target node
        source: source node, not used
        env: environment context, not used
        for_signature: whether this is being done for signature generation

    Returns:
        the directory the library will be in (empty string if '.')
    """
    verbose = False

    if target.dir and str(target.dir) != ".":
        if verbose:
            print("_get_shlib_dir: target.dir:%s" % target.dir)

        return "%s/" % str(target.dir)
    else:
        return ""


def setup_shared_lib_logic(env) -> None:
    """Initialize an environment for shared library building.

    Args:
        env: environment to set up
    """
    createSharedLibBuilder(env)

    env["_get_shlib_stem"] = _get_shlib_stem
    env["_get_shlib_dir"] = _get_shlib_dir
    env["_SHLIBSOVERSION"] = _soversion
    env["_SHLIBSONAME"] = _soname

    env["SHLIBNAME"] = "${_get_shlib_dir}${SHLIBPREFIX}$_get_shlib_stem${_SHLIBSUFFIX}"

    # This is the non versioned shlib filename
    # If SHLIBVERSION is defined then this will symlink to $SHLIBNAME
    env["SHLIB_NOVERSION_SYMLINK"] = "${_get_shlib_dir}${SHLIBPREFIX}$_get_shlib_stem${SHLIBSUFFIX}"

    # This is the sonamed file name
    # If SHLIBVERSION is defined then this will symlink to $SHLIBNAME
    env["SHLIB_SONAME_SYMLINK"] = "${_get_shlib_dir}$_SHLIBSONAME"

    # Note this is gnu style
    env["SHLIBSONAMEFLAGS"] = "-Wl,-soname=$_SHLIBSONAME"
    env["_SHLIBVERSION"] = "${SHLIBVERSION and '.'+SHLIBVERSION or ''}"
    env["_SHLIBVERSIONFLAGS"] = "$SHLIBVERSIONFLAGS -Wl,-soname=$_SHLIBSONAME"

    env["SHLIBEMITTER"] = [lib_emitter, shlib_symlink_emitter]

    # If it's already set, then don't overwrite.
    env["SHLIBPREFIX"] = env.get('SHLIBPREFIX', "lib")
    env["_SHLIBSUFFIX"] = "${SHLIBSUFFIX}${_SHLIBVERSION}"

    env["SHLINKFLAGS"] = CLVar("$LINKFLAGS -shared")

    env["SHLINKCOM"] = "$SHLINK -o $TARGET $SHLINKFLAGS $__SHLIBVERSIONFLAGS $__RPATH $SOURCES $_LIBDIRFLAGS $_LIBFLAGS"

    env["SHLINK"] = "$LINK"
