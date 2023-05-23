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

import inspect, os, re, shutil, sys, unittest

class CompatibilityTestCase(unittest.TestCase):
    '''
    The base class for running compatibility tests.
    '''

    def pr(self, s):
        '''
        Print diagnostic output.
        '''
        print(s)

    def branch_path(self, branch):
        '''
        Get path to a branch.
        '''
        return os.path.abspath(os.path.join('..', branch))

    def module_file(self):
        '''
        Get the (base) file name for this test.
        '''
        return os.path.basename(inspect.getfile(self.__class__))
    
    def module_name(self):
        '''
        Get the module name for this test.
        '''
        return self.module_file().replace('.py', '')

    def system(self, command):
        '''
        Run a command, fail the test if the command execution failed.
        '''
        self.assertEqual(os.system(command), 0)

    def prepare_wt_branch(self, branch):
        '''
        Check out and build another WT branch.
        '''

        # Clone the repository and check out the correct branch
        path = self.branch_path(branch)
        if os.path.exists(path):
            self.pr(f'Branch {branch} is already cloned')
            self.system(f'git -C "{path}" pull')
        else:
            source = 'git@github.com:wiredtiger/wiredtiger.git'
            self.pr(f'Cloning branch {branch}')
            self.system(f'git clone "{source}" "{path}" -b {branch}')

        # Build
        build_path = os.path.join(path, 'build')
        # Note: This build code works only on branches 6.0 and newer. We will need to update it to
        # support older branches, which use autoconf.
        if not os.path.exists(os.path.join(build_path, 'build.ninja')):
            os.mkdir(build_path)
            # Disable WT_STANDALONE_BUILD, because it is not compatible with branches 6.0 and
            # earlier.
            cmake_args = '-DWT_STANDALONE_BUILD=0 -DENABLE_PYTHON=1'
            self.system(f'cd "{build_path}" && cmake {cmake_args} -G Ninja ../.')

        self.pr(f'Building {path}')
        self.system(f'cd "{build_path}" && ninja')

    def run_method_on_branch(self, branch, method):
        '''
        Run a method on a branch.
        '''
        cwd = os.getcwd()
        branch_path = self.branch_path(branch)
        build_python_path = os.path.join(branch_path, 'build', 'lang', 'python')
        this_script_dir = os.path.abspath(os.path.dirname(__file__))

        class_name = self.__class__.__name__
        module_name = self.module_name()

        if not method.endswith(')'):
            method += '()'
        method = method.replace('\'', '"')

        commands = f'import os, sys;'
        commands += f'sys.path.insert(1, "{build_python_path}");'
        commands += f'import {module_name};'
        commands += f'os.chdir("{cwd}");'
        commands += f'sys.exit({module_name}.{class_name}().{method})'

        self.system(f'cd \'{this_script_dir}\' && python3 -c \'{commands}\'')
    
    def short_id(self):
        return self.id().replace('__main__.', '')

    def sanitized_short_id(self):
        '''
        Return a name that is suitable for creating file system names. In particular, names with
        scenarios that look like 'test_file.test_file.test_funcname(scen1.scen2.scen3)'. So
        transform '(', but remove final ')'.
        '''
        name = self.short_id().translate(str.maketrans('($[]/ ','______', ')'))

        # On OS/X, we can get name conflicts if names differ by case. Upper
        # case letters are uncommon in our python class and method names, so
        # we lowercase them and prefix with '@', e.g. "AbC" -> "@ab@c".
        return re.sub(r'[A-Z]', lambda x: '@' + x.group(0).lower(), name)

    def setUp(self):
        '''
        Setup the test.
        '''
        self._start_dir = os.getcwd()

        self._test_dir = os.path.join('compatibility-test', self.sanitized_short_id())
        shutil.rmtree(self._test_dir, ignore_errors=True)

        if os.path.exists(self._test_dir):
            raise Exception(self._test_dir + ": cannot remove directory")
        os.makedirs(self._test_dir)
        os.chdir(self._test_dir)

    def tearDown(self):
        '''
        Tear down the test.
        '''
        os.chdir(self._start_dir)

def run_suite(suite):
    '''
    Run the test suite.
    '''
    try:
        result = unittest.TextTestRunner(verbosity=1, resultclass=None).run(suite)
        return result
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print("[pid:{}]: ERROR: running test: {}".format(os.getpid(), e))
        raise e

def run(name='__main__'):
    '''
    Run the test.
    '''
    result = run_suite(unittest.TestLoader().loadTestsFromName(name))
    sys.exit(0 if result.wasSuccessful() else 1)
