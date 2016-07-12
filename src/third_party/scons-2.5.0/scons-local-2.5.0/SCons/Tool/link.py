"""SCons.Tool.link

Tool-specific initialization for the generic Posix linker.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.

"""

#
# Copyright (c) 2001 - 2016 The SCons Foundation
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

__revision__ = "src/engine/SCons/Tool/link.py rel_2.5.0:3543:937e55cd78f7 2016/04/09 11:29:54 bdbaddog"

import sys
import re
import os

import SCons.Tool
import SCons.Util
import SCons.Warnings

from SCons.Tool.FortranCommon import isfortran

from SCons.Tool.DCommon import isD

cplusplus = __import__('c++', globals(), locals(), [])

issued_mixed_link_warning = False

def smart_link(source, target, env, for_signature):
    has_cplusplus = cplusplus.iscplusplus(source)
    has_fortran = isfortran(env, source)
    has_d = isD(env, source)
    if has_cplusplus and has_fortran and not has_d:
        global issued_mixed_link_warning
        if not issued_mixed_link_warning:
            msg = "Using $CXX to link Fortran and C++ code together.\n\t" + \
              "This may generate a buggy executable if the '%s'\n\t" + \
              "compiler does not know how to deal with Fortran runtimes."
            SCons.Warnings.warn(SCons.Warnings.FortranCxxMixWarning,
                                msg % env.subst('$CXX'))
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

def _lib_emitter(target, source, env, **kw):
    Verbose = False
    if Verbose:
        print "_lib_emitter: target[0]=%r" % target[0].get_path()
    for tgt in target:
        tgt.attributes.shared = 1
    
    try:
        symlink_generator = kw['symlink_generator']
    except KeyError:
        pass
    else:
        if Verbose:
            print "_lib_emitter: symlink_generator=%r" % symlink_generator
        symlinks = symlink_generator(env, target[0])
        if Verbose:
            print "_lib_emitter: symlinks=%r" % symlinks 

        if symlinks:
            SCons.Tool.EmitLibSymlinks(env, symlinks, target[0])
            target[0].attributes.shliblinks = symlinks
    return (target, source)

def shlib_emitter(target, source, env):
    return _lib_emitter(target, source, env, symlink_generator = SCons.Tool.ShLibSymlinkGenerator)

def ldmod_emitter(target, source, env):
    return _lib_emitter(target, source, env, symlink_generator = SCons.Tool.LdModSymlinkGenerator)

# This is generic enough to be included here...
def _versioned_lib_name(env, libnode, version, prefix, suffix, prefix_generator, suffix_generator, **kw):
    """For libnode='/optional/dir/libfoo.so.X.Y.Z' it returns 'libfoo.so'"""
    Verbose = False

    if Verbose:
        print "_versioned_lib_name: libnode=%r" % libnode.get_path()
        print "_versioned_lib_name: version=%r" % version
        print "_versioned_lib_name: prefix=%r" % prefix
        print "_versioned_lib_name: suffix=%r" % suffix
        print "_versioned_lib_name: suffix_generator=%r" % suffix_generator

    versioned_name = os.path.basename(libnode.get_path())
    if Verbose:
        print "_versioned_lib_name: versioned_name=%r" % versioned_name

    versioned_prefix = prefix_generator(env, **kw)
    versioned_suffix = suffix_generator(env, **kw)
    if Verbose:
        print "_versioned_lib_name: versioned_prefix=%r" % versioned_prefix
        print "_versioned_lib_name: versioned_suffix=%r" % versioned_suffix

    versioned_prefix_re = '^' + re.escape(versioned_prefix)
    versioned_suffix_re = re.escape(versioned_suffix) + '$'
    name = re.sub(versioned_prefix_re, prefix, versioned_name)
    name = re.sub(versioned_suffix_re, suffix, name)
    if Verbose:
        print "_versioned_lib_name: name=%r" % name
    return name

def _versioned_shlib_name(env, libnode, version, prefix, suffix, **kw):
    pg = SCons.Tool.ShLibPrefixGenerator
    sg = SCons.Tool.ShLibSuffixGenerator
    return _versioned_lib_name(env, libnode, version, prefix, suffix, pg, sg, **kw)

def _versioned_ldmod_name(env, libnode, version, prefix, suffix, **kw):
    pg = SCons.Tool.LdModPrefixGenerator
    sg = SCons.Tool.LdModSuffixGenerator
    return _versioned_lib_name(env, libnode, version, prefix, suffix, pg, sg, **kw)

def _versioned_lib_suffix(env, suffix, version):
    """For suffix='.so' and version='0.1.2' it returns '.so.0.1.2'"""
    Verbose = False
    if Verbose:
        print "_versioned_lib_suffix: suffix=%r" % suffix
        print "_versioned_lib_suffix: version=%r" % version
    if not suffix.endswith(version):
        suffix = suffix + '.' + version
    if Verbose:
        print "_versioned_lib_suffix: return suffix=%r" % suffix
    return suffix

def _versioned_lib_soname(env, libnode, version, prefix, suffix, name_func):
    """For libnode='/optional/dir/libfoo.so.X.Y.Z' it returns 'libfoo.so.X'"""
    Verbose = False
    if Verbose:
        print "_versioned_lib_soname: version=%r" % version
    name = name_func(env, libnode, version, prefix, suffix)
    if Verbose:
        print "_versioned_lib_soname: name=%r" % name
    major = version.split('.')[0]
    soname = name + '.' + major
    if Verbose:
        print "_versioned_lib_soname: soname=%r" % soname
    return soname

def _versioned_shlib_soname(env, libnode, version, prefix, suffix):
    return _versioned_lib_soname(env, libnode, version, prefix, suffix, _versioned_shlib_name) 

def _versioned_ldmod_soname(env, libnode, version, prefix, suffix):
    return _versioned_lib_soname(env, libnode, version, prefix, suffix, _versioned_ldmod_name) 

def _versioned_lib_symlinks(env, libnode, version, prefix, suffix, name_func, soname_func):
    """Generate link names that should be created for a versioned shared lirbrary.
       Returns a dictionary in the form { linkname : linktarget }
    """
    Verbose = False

    if Verbose:
        print "_versioned_lib_symlinks: libnode=%r" % libnode.get_path()
        print "_versioned_lib_symlinks: version=%r" % version

    if sys.platform.startswith('openbsd'):
        # OpenBSD uses x.y shared library versioning numbering convention
        # and doesn't use symlinks to backwards-compatible libraries
        if Verbose:
            print "_versioned_lib_symlinks: return symlinks=%r" % None
        return None

    linkdir = libnode.get_dir()
    if Verbose:
        print "_versioned_lib_symlinks: linkdir=%r" % linkdir.get_path()

    name = name_func(env, libnode, version, prefix, suffix)
    if Verbose:
        print "_versioned_lib_symlinks: name=%r" % name

    soname = soname_func(env, libnode, version, prefix, suffix)

    link0 = env.fs.File(soname, linkdir)
    link1 = env.fs.File(name, linkdir)

    # We create direct symlinks, not daisy-chained.
    if link0 == libnode:
        # This enables SHLIBVERSION without periods (e.g. SHLIBVERSION=1)
        symlinks = [ (link1, libnode) ]
    else:
        # This handles usual SHLIBVERSION, i.e. '1.2', '1.2.3', etc.
        symlinks = [ (link0, libnode), (link1, libnode) ]

    if Verbose:
        print "_versioned_lib_symlinks: return symlinks=%r" % SCons.Tool.StringizeLibSymlinks(symlinks)

    return symlinks

def _versioned_shlib_symlinks(env, libnode, version, prefix, suffix):
    nf = _versioned_shlib_name
    sf = _versioned_shlib_soname
    return _versioned_lib_symlinks(env, libnode, version, prefix, suffix, nf, sf)

def _versioned_ldmod_symlinks(env, libnode, version, prefix, suffix):
    nf = _versioned_ldmod_name
    sf = _versioned_ldmod_soname
    return _versioned_lib_symlinks(env, libnode, version, prefix, suffix, nf, sf)

def _versioned_lib_callbacks():
    return {
        'VersionedShLibSuffix'   : _versioned_lib_suffix,
        'VersionedLdModSuffix'   : _versioned_lib_suffix,
        'VersionedShLibSymlinks' : _versioned_shlib_symlinks,
        'VersionedLdModSymlinks' : _versioned_ldmod_symlinks,
        'VersionedShLibName'     : _versioned_shlib_name,
        'VersionedLdModName'     : _versioned_ldmod_name,
        'VersionedShLibSoname'   : _versioned_shlib_soname,
        'VersionedLdModSoname'   : _versioned_ldmod_soname,
    }.copy()

# Setup all variables required by the versioning machinery
def _setup_versioned_lib_variables(env, **kw):

    tool = None
    try: tool = kw['tool']
    except KeyError: pass

    use_soname = False
    try: use_soname = kw['use_soname'] 
    except KeyError: pass

    # The $_SHLIBVERSIONFLAGS define extra commandline flags used when
    # building VERSIONED shared libraries. It's always set, but used only
    # when VERSIONED library is built (see __SHLIBVERSIONFLAGS in SCons/Defaults.py).
    if use_soname:
        # If the linker uses SONAME, then we need this little automata
        if tool == 'sunlink':
            env['_SHLIBVERSIONFLAGS'] = '$SHLIBVERSIONFLAGS -h $_SHLIBSONAME'
            env['_LDMODULEVERSIONFLAGS'] = '$LDMODULEVERSIONFLAGS -h $_LDMODULESONAME'
        else:
            env['_SHLIBVERSIONFLAGS'] = '$SHLIBVERSIONFLAGS -Wl,-soname=$_SHLIBSONAME'
            env['_LDMODULEVERSIONFLAGS'] = '$LDMODULEVERSIONFLAGS -Wl,-soname=$_LDMODULESONAME'
        env['_SHLIBSONAME'] = '${ShLibSonameGenerator(__env__,TARGET)}'
        env['_LDMODULESONAME'] = '${LdModSonameGenerator(__env__,TARGET)}'
        env['ShLibSonameGenerator'] = SCons.Tool.ShLibSonameGenerator
        env['LdModSonameGenerator'] = SCons.Tool.LdModSonameGenerator
    else:
        env['_SHLIBVERSIONFLAGS'] = '$SHLIBVERSIONFLAGS'
        env['_LDMODULEVERSIONFLAGS'] = '$LDMODULEVERSIONFLAGS'

    # LDOMDULVERSIONFLAGS should always default to $SHLIBVERSIONFLAGS
    env['LDMODULEVERSIONFLAGS'] = '$SHLIBVERSIONFLAGS'


def generate(env):
    """Add Builders and construction variables for gnulink to an Environment."""
    SCons.Tool.createSharedLibBuilder(env)
    SCons.Tool.createProgBuilder(env)

    env['SHLINK']      = '$LINK'
    env['SHLINKFLAGS'] = SCons.Util.CLVar('$LINKFLAGS -shared')
    env['SHLINKCOM']   = '$SHLINK -o $TARGET $SHLINKFLAGS $__SHLIBVERSIONFLAGS $__RPATH $SOURCES $_LIBDIRFLAGS $_LIBFLAGS'
    # don't set up the emitter, cause AppendUnique will generate a list
    # starting with None :-(
    env.Append(SHLIBEMITTER = [shlib_emitter])
    env['SMARTLINK']   = smart_link
    env['LINK']        = "$SMARTLINK"
    env['LINKFLAGS']   = SCons.Util.CLVar('')
    # __RPATH is only set to something ($_RPATH typically) on platforms that support it.
    env['LINKCOM']     = '$LINK -o $TARGET $LINKFLAGS $__RPATH $SOURCES $_LIBDIRFLAGS $_LIBFLAGS'
    env['LIBDIRPREFIX']='-L'
    env['LIBDIRSUFFIX']=''
    env['_LIBFLAGS']='${_stripixes(LIBLINKPREFIX, LIBS, LIBLINKSUFFIX, LIBPREFIXES, LIBSUFFIXES, __env__)}'
    env['LIBLINKPREFIX']='-l'
    env['LIBLINKSUFFIX']=''

    if env['PLATFORM'] == 'hpux':
        env['SHLIBSUFFIX'] = '.sl'
    elif env['PLATFORM'] == 'aix':
        env['SHLIBSUFFIX'] = '.a'

    # For most platforms, a loadable module is the same as a shared
    # library.  Platforms which are different can override these, but
    # setting them the same means that LoadableModule works everywhere.
    SCons.Tool.createLoadableModuleBuilder(env)
    env['LDMODULE'] = '$SHLINK'
    env.Append(LDMODULEEMITTER = [ldmod_emitter])
    env['LDMODULEPREFIX'] = '$SHLIBPREFIX'
    env['LDMODULESUFFIX'] = '$SHLIBSUFFIX'
    env['LDMODULEFLAGS'] = '$SHLINKFLAGS'
    env['LDMODULECOM'] = '$LDMODULE -o $TARGET $LDMODULEFLAGS $__LDMODULEVERSIONFLAGS $__RPATH $SOURCES $_LIBDIRFLAGS $_LIBFLAGS'
    env['LDMODULEVERSION'] = '$SHLIBVERSION'
    env['LDMODULENOVERSIONSYMLINKS'] = '$SHLIBNOVERSIONSYMLINKS'

def exists(env):
    # This module isn't really a Tool on its own, it's common logic for
    # other linkers.
    return None

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
