#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

# This script builds a Python source distribution that can built be installed
# via pip install. This must be run in a git repository to determine the files
# to package. Also as a prerequisite, SWIG must be run as the generated files
# are part of the package. To create the distribution, in this directory, run
# "python setup_pip.py sdist", this creates a tar.gz file under ./dist .
from __future__ import print_function
import os, os.path, re, shutil, site, sys
from setuptools import setup, Distribution
from distutils.extension import Extension
import distutils.sysconfig
import distutils.ccompiler
from distutils.errors import CompileError, LinkError
import subprocess
from subprocess import call
import setuptools.command.install
import setuptools.command.build_ext

# msg --
#   Print a message to stderr.
def msg(s):
    print(os.path.basename(__file__) + ": " + s, file=sys.stderr)

# die --
#   For failures, show a message and exit.
def die(s):
    msg(s)
    sys.exit(1)

# build_commands --
#   Run a sequence of commands, and die if any fail.
def build_commands(commands, build_dir, build_env):
    for command in commands:
        callargs = [ 'sh', '-c', command ]
        verbose_command = '"' + '" "'.join(callargs) + '"'
        print('running: ' + verbose_command)
        if call(callargs, cwd=build_dir, env=build_env) != 0:
            die('build command failed: ' + verbose_command)

# check_needed_dependencies --
#   Make a quick check of any needed library dependencies, and
# add to the library path and include path as needed.  If a library
# is not found, it is not definitive.
def check_needed_dependencies(builtins, inc_paths, lib_paths):
    library_dirs = get_library_dirs()
    compiler = distutils.ccompiler.new_compiler()
    distutils.sysconfig.customize_compiler(compiler)
    compiler.set_library_dirs(library_dirs)
    missing = []
    for name, libname, instructions in builtins:
        found = compiler.find_library_file(library_dirs, libname)
        if found is None:
            msg(libname + ": missing")
            msg(instructions)
            msg("after installing it, set LD_LIBRARY_PATH or DYLD_LIBRARY_PATH")
            missing.append(libname)
        else:
            package_top = os.path.dirname(os.path.dirname(found))
            inc_paths.append(os.path.join(package_top, 'include'))
            lib_paths.append(os.path.join(package_top, 'lib'))

    # XXX: we are not accounting for other directories that might be
    # discoverable via /sbin/ldconfig. It might be better to write a tiny
    # compile using  -lsnappy, -lz...
    #
    #if len(missing) > 0:
    #    die("install packages for: " + str(missing))

# find_executable --
#   Locate an executable in the PATH.
def find_executable(exename, path):
    p = subprocess.Popen(['which', exename ], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    out, err = p.communicate('')
    out = str(out)  # needed for Python3
    if out == '':
        if err != '':
            err = ': "' + err + '"'
        die('"' + exename + '": not found in path' + err)
    dirname = os.path.dirname(out)
    if not dirname in path:
        path.append(dirname)

# get_build_path --
#   Create a PATH that can be used for installation.  Apparently,
# installation commands are run with a restricted PATH, and
# autoreconf/aclocal will not normally be found.
def get_build_path():
    build_paths = []
    find_executable('autoreconf', build_paths)
    find_executable('aclocal', build_paths)
    build_path = os.environ['PATH'] + ':' + ':'.join(build_paths)
    return build_path

# get_compile_flags --
#   Get system specific compile flags.  Return a triple: C preprocessor
# flags, C compilation flags and linker flags.
def get_compile_flags(inc_paths, lib_paths):
    # Suppress warnings building SWIG generated code
    if sys.platform == 'win32' and cc == 'msvc':
        cflags = ['/arch:SSE2', '/EHsc']
        cppflags = []
        ldflags = []
        # Windows untested and incomplete, don't claim that it works.
        die('Windows is not supported by this setup script')
    else:
        cflags = [ '-w', '-Wno-sign-conversion', '-std=c11' ]
        cppflags = ['-I' + path for path in inc_paths]
        cppflags.append('-DHAVE_CONFIG_H')
        ldflags = ['-L' + path for path in lib_paths]
        if sys.platform == 'darwin':
            cflags.extend([ '-arch', 'x86_64' ])
    return (cppflags, cflags, ldflags)

# get_sources_curdir --
#   Get a list of sources from the current directory
def get_sources_curdir():
    DEVNULL = open(os.devnull, 'w')
    gitproc = subprocess.Popen(
        ['git', 'ls-tree', '-r', '--name-only', 'HEAD^{tree}'],
        stdin=DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sources = [line.rstrip() for line in gitproc.stdout.readlines()]
    err = gitproc.stderr.read()
    gitproc.wait()
    subret = gitproc.returncode
    if subret != 0 or err:
        msg("git command to get sources returned " + str(subret) +
            ", error=" + str(err))
        die("this command must be run in a git repository")
    return sources

# get_wiredtiger_versions --
#   Read the version information from the RELEASE_INFO file.
def get_wiredtiger_versions(wt_dir):
    v = {}
    for l in open(os.path.join(wt_dir, 'RELEASE_INFO')):
        if re.match(r'WIREDTIGER_VERSION_(?:MAJOR|MINOR|PATCH)=', l):
            exec(l, v)
    wt_ver = '%d.%d' % (v['WIREDTIGER_VERSION_MAJOR'],
                        v['WIREDTIGER_VERSION_MINOR'])
    wt_full_ver = wt_ver + '.%d' % (v['WIREDTIGER_VERSION_PATCH'])
    return (wt_ver, wt_full_ver)

# get_library_dirs
#   Build a plausible set of library directories.
def get_library_dirs():
    dirs = []
    dirs.append("/usr/local/lib")
    dirs.append("/usr/local/lib64")
    dirs.append("/lib/x86_64-linux-gnu")
    dirs.append("/opt/local/lib")
    dirs.append("/usr/lib")
    dirs.append("/usr/lib64")
    for path in ['LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'LIBRARY_PATH']:
        if path in os.environ:
            dirs.extend(os.environ[path].split(':'))
    dirs = list(set(filter(os.path.isdir, dirs)))
    return dirs

# source_filter
#   Make any needed changes to the sources list.  Any entry that
# needs to be moved is returned in a dictionary.
def source_filter(sources):
    result = []
    movers = dict()
    py_dir = os.path.join('lang', 'python')
    pywt_dir = os.path.join(py_dir, 'wiredtiger')
    pywt_prefix = pywt_dir + os.path.sep
    for f in sources:
        if not re.match(source_regex, f):
            continue
        src = f
        dest = f
        # move all lang/python files to the top level.
        if dest.startswith(pywt_prefix):
            dest = os.path.basename(dest)
            if dest == 'pip_init.py':
                dest = '__init__.py'
        if dest != src:
            movers[dest] = src
        result.append(dest)
    # Add SWIG generated files
    result.append('wiredtiger.py')
    movers['wiredtiger.py'] = os.path.join(pywt_dir, '__init__.py')
    result.append(os.path.join(py_dir, 'wiredtiger_wrap.c'))
    return result, movers

################################################################
# Do some initial setup and checks.
this_abs_script = os.path.abspath(__file__)
this_dir = os.path.dirname(this_abs_script)
pip_command = None
for arg in sys.argv[1:]:
    if arg[0] != '-' and pip_command == None:
        pip_command = arg
        break

if this_dir.endswith(os.sep + os.path.join('lang', 'python')):
    wt_dir = os.path.dirname(os.path.dirname(this_dir))
    os.chdir(wt_dir)
elif os.path.isfile(os.path.join(this_dir, 'LICENSE')):
    wt_dir = this_dir
else:
    die('running from an unknown directory')

python3 = (sys.version_info[0] > 2)
if python3:
    die('Python3 is not yet supported')

# Ensure that Extensions won't be built for 32 bit,
# that won't work with WiredTiger.
if sys.maxsize < 2**32:
    die('need to be running on a 64 bit system, and have a 64 bit Python')

python_rel_dir = os.path.join('lang', 'python')
build_dir = os.path.join(wt_dir, 'build_posix')
makefile = os.path.join(build_dir, 'Makefile')
built_sentinal = os.path.join(build_dir, 'built.txt')
conf_make_dir = 'build_posix'
wt_swig_lib_name = os.path.join(python_rel_dir, '_wiredtiger.so')

################################################################
# Put together build options for the WiredTiger extension.
short_description = 'high performance, scalable, production quality, ' + \
    'NoSQL, Open Source extensible platform for data management'
long_description = 'WiredTiger is a ' + short_description + '.\n\n' + \
    open(os.path.join(wt_dir, 'README')).read()

wt_ver, wt_full_ver = get_wiredtiger_versions(wt_dir)
build_path = get_build_path()

# We only need a small set of directories to build a WT library,
# we also include any files at the top level.
source_regex = r'^(?:(?:api|build_posix|ext|lang/python|src|dist)/|[^/]*$)'

# The builtins that we include in this distribution.
builtins = [
    # [ name, libname, instructions ]
    [ 'snappy', 'snappy',
      'Note: a suitable version of snappy can be found at\n' + \
      '    https://github.com/google/snappy/releases/download/' + \
      '1.1.3/snappy-1.1.3.tar.gz\n' + \
      'It can be installed via: yum install snappy snappy-devel' + \
      'or via: apt-get install libsnappy-dev' ],
    [ 'zlib', 'z',
      'Need to install zlib\n' + \
      'It can be installed via: apt-get install zlib1g' ]
]
builtin_names = [b[0] for b in builtins]
builtin_libraries = [b[1] for b in builtins]

# Here's the configure/make operations we perform before the python extension
# is linked.
configure_cmds = [
    './makemake --clean-and-make',
    './reconf',
    # force building a position independent library; it will be linked
    # into a single shared library with the SWIG interface code.
    'CFLAGS="${CFLAGS:-} -fPIC -DPIC" ' + \
    '../configure --enable-python --with-builtins=' + ','.join(builtin_names)
]

# build all the builtins, at the moment they are all compressors.
make_cmds = []
for name in builtin_names:
    make_cmds.append('(cd ext/compressors/' + name + '/; make)')
make_cmds.append('make libwiredtiger.la')

inc_paths = [ os.path.join(build_dir, 'src', 'include'), build_dir, '.' ]
lib_paths = [ '.' ]   # wiredtiger.so is moved into the top level directory

check_needed_dependencies(builtins, inc_paths, lib_paths)

cppflags, cflags, ldflags = get_compile_flags(inc_paths, lib_paths)

# If we are creating a source distribution, create a staging directory
# with just the right sources. Put the result in the python dist directory.
if pip_command == 'sdist':
    sources, movers = source_filter(get_sources_curdir())
    stage_dir = os.path.join(python_rel_dir, 'stage')
    shutil.rmtree(stage_dir, True)
    os.makedirs(stage_dir)
    shutil.copy2(this_abs_script, os.path.join(stage_dir, 'setup.py'))
    for f in sources:
        d = os.path.join(stage_dir, os.path.dirname(f))
        if not os.path.isdir(d):
            os.makedirs(d)
        if f in movers:
            src = movers[f]
        else:
            src = f
        # Symlinks are not followed in setup, we need to use real files.
        shutil.copy2(src, os.path.join(stage_dir, f))
    os.chdir(stage_dir)
    sys.argv.append('--dist-dir=' + os.path.join('..', 'dist'))
else:
    sources = [ os.path.join(python_rel_dir, 'wiredtiger_wrap.c') ]

wt_ext = Extension('_wiredtiger',
    sources = sources,
    extra_compile_args = cflags + cppflags,
    extra_link_args = ldflags,
    libraries = builtin_libraries,
    extra_objects = [ os.path.join(build_dir, '.libs', 'libwiredtiger.a') ],
    include_dirs = inc_paths,
    library_dirs = lib_paths,
)
extensions = [ wt_ext ]
env = { "CFLAGS" : ' '.join(cflags),
        "CPPFLAGS" : ' '.join(cppflags),
        "LDFLAGS" : ' '.join(ldflags),
        "PATH" : build_path }

class BinaryDistribution(Distribution):
    def is_pure(self):
        return False

class WTInstall(setuptools.command.install.install):
    def run(self):
        self.run_command("build_ext")
        return setuptools.command.install.install.run(self)

class WTBuildExt(setuptools.command.build_ext.build_ext):
    def __init__(self, *args, **kwargs):
        setuptools.command.build_ext.build_ext.__init__(self, *args, **kwargs)

    def run(self):
        # only run this once
        if not os.path.isfile(built_sentinal):
            try:
                os.remove(makefile)
            except OSError:
                pass
            self.execute(
                lambda: build_commands(configure_cmds, conf_make_dir, env), [],
                'wiredtiger configure')
            if not os.path.isfile(makefile):
                die('configure failed, file does not exist: ' + makefile)
            self.execute(
                lambda: build_commands(make_cmds, conf_make_dir, env), [],
                'wiredtiger make')
            open(built_sentinal, 'a').close()
        return setuptools.command.build_ext.build_ext.run(self)

setup(
    name = 'wiredtiger',
    version = wt_full_ver,
    author = 'The WiredTiger Development Team, part of MongoDB',
    author_email = 'info@wiredtiger.com',
    description = short_description,
    license='GPL2,GPL3,Commercial',
    long_description = long_description,
    url = 'http://source.wiredtiger.com/',
    keywords = 'scalable NoSQL database datastore engine open source',
    packages = ['wiredtiger'],
    ext_package = 'wiredtiger',
    ext_modules = extensions,
    include_package_data = True,
    distclass = BinaryDistribution,
    package_dir = { 'wiredtiger' : '.' },
    cmdclass = { 'install': WTInstall, 'build_ext': WTBuildExt },
    package_data = {
        'wiredtiger' : [ wt_swig_lib_name, '*.py' ]
    },
    classifiers=[
        'Intended Audience :: Developers',
        'Programming Language :: C',
        'Programming Language :: C++',
        'Programming Language :: Python',
        'Programming Language :: Java',
        'Operating System :: MacOS :: MacOS X',
        'Operating System :: POSIX',
        'Operating System :: POSIX :: BSD',
        'Operating System :: POSIX :: Linux',
        'Operating System :: POSIX :: SunOS/Solaris',
    ]
)

if pip_command == 'sdist':
    shutil.rmtree(os.path.join(this_dir, 'stage'))
