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

import os, re, sys
from compatibility_config import BRANCHES, BRANCHES_DIR

# Remember the top-level test directory and the top-level project directory.
TEST_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DIST_TOP_DIR = os.path.abspath(os.path.join(TEST_DIR, '..'))

# Set up Python import paths, so that we can find the core dependencies.
sys.path.insert(1, os.path.join(TEST_DIR, 'py_utility'))

# Now set up the rest of the paths, so that we can access our third-party dependencies.
# IMPORTANT: Don't import wiredtiger until you get to the actual test, so that you don't
# accidentally import the wrong version of the library.
import test_util
test_util.setup_3rdparty_paths()

# The default build directory
DEFAULT_BUILD_DIR = 'build.compatibility'

def branch_path(branch):
    '''
    Get path to a branch.
    '''
    if branch == 'this':
        return DIST_TOP_DIR
    return os.path.abspath(os.path.join(DIST_TOP_DIR, BRANCHES_DIR, branch))

def branch_build_path(branch, config):
    '''
    Get path to the build directory within a branch.
    '''
    dir = os.path.join(branch_path(branch), DEFAULT_BUILD_DIR)
    if config is not None:
        config_keys = list(config.keys())
        config_keys.sort()
        for k in config_keys:
            v = config[k]
            if v is False:
                continue
            if v is True:
                dir += f'-{k}'
                continue
            str_v = re.sub(r'[/\s\\\'\"\*\?]', '-', str(v))
            dir += f'-{k}={str_v}'
    return dir

def system(command):
    '''
    Run a command, fail if the command execution failed.
    '''
    r = os.system(command)
    if r != 0:
        raise Exception('Command \'%s\' failed with code %d.' % (command, r))

def prepare_branch(branch, config):
    '''
    Check out and build a WiredTiger branch.
    '''

    # Clone the repository and check out the correct branch.
    path = branch_path(branch)
    if branch != 'this':
        if os.path.exists(path):
            print(f'Branch {branch} is already cloned')
            system(f'git -C "{path}" pull')
        else:
            source = 'https://github.com/wiredtiger/wiredtiger.git'
            print(f'Cloning branch {branch}')
            system(f'git clone "{source}" "{path}" -b {branch}')

    # Parse the build config
    standalone = False
    if config is not None:
        for key, value in config.items():
            if key == 'standalone':
                standalone = value
            else:
                raise Exception(f'Unsupported build configuration key \'{key}\'')

    # Build
    build_path = branch_build_path(branch, config)
    # Note: This build code works only on branches 6.0 and newer. We will need to update it to
    # support older branches, which use autoconf.
    if not os.path.exists(os.path.join(build_path, 'build.ninja')):
        os.mkdir(build_path)
        cmake_args = '-DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/mongodbtoolchain_stable_gcc.cmake'
        cmake_args += ' -DENABLE_PYTHON=1'
        if not standalone:
            cmake_args += ' -DWT_STANDALONE_BUILD=0'
        system(f'cd "{build_path}" && cmake {cmake_args} -G Ninja ../.')

    print(f'Building {path}')
    system(f'cd "{build_path}" && ninja')
