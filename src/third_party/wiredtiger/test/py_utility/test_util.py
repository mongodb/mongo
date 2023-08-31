#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
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

import os, sys

def get_dist_top_dir():
    '''
    Get the top-level directory.
    '''
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def find_build_dir():
    '''
    Find the build directory.
    '''

    wt_disttop = get_dist_top_dir()

    # Check for a local build that contains the wt utility. First check if the
    # supplied an explicit build directory ('WT_BUILDDIR'), then the current
    # working directory, and finally in the disttop directory.
    # This isn't ideal - if a user has multiple builds in a tree we
    # could pick the wrong one. We also need to account for the fact that there
    # may be an executable 'wt' file the build directory.

    def is_build_dir(dir):
        '''
        Check whether the provided directory is the build directory.
        '''
        return os.path.isfile(os.path.join(dir, 'wt')) or \
               os.path.isfile(os.path.join(dir, 'wt.exe'))

    env_builddir = os.getenv('WT_BUILDDIR')
    curdir = os.getcwd()
    if env_builddir and os.path.isfile(os.path.join(env_builddir, 'wt')):
        wt_builddir = env_builddir
    elif is_build_dir(curdir):
        wt_builddir = curdir
    elif is_build_dir(wt_disttop):
        wt_builddir = wt_disttop
    elif is_build_dir(os.path.join(wt_disttop, 'build')):
        wt_builddir = os.path.join(wt_disttop, 'build')
    else:
        print('Unable to find useable WiredTiger build')
        sys.exit(1)
    return wt_builddir

def setup_wiredtiger_path():
    '''
    Set up Python import path for WiredTiger.
    '''

    wt_builddir = find_build_dir()

    # Cannot import wiredtiger and supporting utils until we set up paths
    # We want our local tree in front of any installed versions of WiredTiger.

    sys.path.insert(1, os.path.join(wt_builddir, 'lang', 'python'))

    # Append to a colon separated path in the environment
    def append_env_path(name, value):
        path = os.environ.get(name)
        if path == None:
            v = value
        else:
            v = path + ':' + value
        os.environ[name] = v

    # If we built with libtool, explicitly put its install directory in our library
    # search path. This only affects library loading for subprocesses, like 'wt'.
    libsdir = os.path.join(wt_builddir, '.libs')
    if os.path.isdir(libsdir):
        append_env_path('LD_LIBRARY_PATH', libsdir)
        if sys.platform == "darwin":
            append_env_path('DYLD_LIBRARY_PATH', libsdir)

def setup_3rdparty_paths():
    '''
    Set up Python import paths for the tests' third party dependencies.
    '''

    wt_3rdpartydir = os.path.join(get_dist_top_dir(), 'test', '3rdparty')

    # Add all 3rd party directories: some have code in subdirectories
    for d in os.listdir(wt_3rdpartydir):
        for subdir in ('lib', 'python', ''):
            if os.path.exists(os.path.join(wt_3rdpartydir, d, subdir)):
                sys.path.insert(1, os.path.join(wt_3rdpartydir, d, subdir))
                break

def setup_paths():
    '''
    Set up Python import paths.
    '''
    setup_wiredtiger_path()
    setup_3rdparty_paths()
