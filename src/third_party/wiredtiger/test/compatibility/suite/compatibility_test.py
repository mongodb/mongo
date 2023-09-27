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

import argparse, inspect, os, pickle, shutil, subprocess, sys, tempfile, unittest

# Import the common compatibility test functionality
sys.path.insert(1, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'common'))
import compatibility_common

# Now we can import our test/py_utility and third-party dependencies
import abstract_test_case, testtools, test_result, test_util, wtscenario

class CompatibilityTestCase(abstract_test_case.AbstractWiredTigerTestCase):
    '''
    The base class for running compatibility tests.
    '''

    def __init__(self, *args, **kwargs):
        '''
        Initialize the test case.
        '''
        super().__init__(*args, **kwargs)
        # Nothing else to do at this point

    def branch_build_path(self, branch):
        '''
        Get path to a branch.
        '''
        build_config = self.build_config if hasattr(self, 'build_config') else None
        return compatibility_common.branch_build_path(branch, build_config)

    def run_method_on_branch(self, branch, method):
        '''
        Run a method on a branch.
        '''
        cwd = os.getcwd()
        branch_path = self.branch_build_path(branch)
        build_python_path = os.path.join(branch_path, 'lang', 'python')
        this_script_dir = os.path.abspath(os.path.dirname(__file__))

        class_name = self.__class__.__name__
        module_name = self.module_name()
        if not method.endswith(')'):
            method += '()'

        # Prepare the script file to run: Set up the right paths, instantiate the test, copy the
        # test's fields, and run the test.
        with tempfile.NamedTemporaryFile(mode='w', suffix='.py', prefix='test-script-',
                                         dir=self._test_dir, delete=False) as f:
            script_path = f.name
            f.write(f'#!/usr/bin/env python\n')
            f.write(f'\n')

            # Set up paths and libraries.
            f.write(f'import os, pickle, sys\n')
            f.write(f'sys.path[0] = "{this_script_dir}"\n')
            f.write(f'sys.path.insert(1, "{build_python_path}")\n')
            f.write(f'\n')
            f.write(f'import wiredtiger\n')
            f.write(f'import {module_name}\n')
            f.write(f'\n')
            f.write(f'os.chdir("{cwd}")\n')
            f.write(f'\n')

            # Set up the class attributes. This is hard, because (1) we need to skip several kinds
            # of attributes, and (2) we need to set up the attributes at the right level of class
            # hierarchy.
            already_exported = dict()
            def skip(k, v):
                module = inspect.getmodule(type(v)).__name__
                if k in already_exported:
                    return True
                if k.startswith('__'):
                    return True
                if callable(v):
                    return True
                if module.startswith('unittest') or module == 'io':
                    return True
                if k == 'captureout' or k == 'captureerr':
                    return True
                return False

            def export_class_attributes(cls):
                if cls is object:
                    return
                export_class_attributes(cls.__base__)
                f.write(f"import {cls.__module__}\n")
                full_class_name = f'{cls.__module__}.{cls.__qualname__}'
                for k, v in inspect.getmembers(cls):
                    if skip(k, v):
                        continue
                    already_exported[k] = True
                    f.write(f"setattr({full_class_name}, '{k}', pickle.loads({pickle.dumps(v)}))\n")

            export_class_attributes(self.__class__)
            f.write(f'\n')

            # Set up the test class instance.
            f.write(f'test = {module_name}.{class_name}()\n')

            # Set up the instance variables.
            for k, v in inspect.getmembers(self):
                if k not in self.__dict__ or skip(k, v):
                    continue
                already_exported[k] = True
                f.write(f"setattr(test, '{k}', pickle.loads({pickle.dumps(v)}))\n")
            f.write(f'\n')

            # We just copied a lot of internal state; now finish the relevant setup that could not
            # be simply copied, e.g., opening the results file.
            f.write(f'test.finishSetupIO()\n')

            # Run the test.
            f.write(f'sys.exit(test.{method})\n')

        # Run the script in a separate Python process, so that it loads the right WT library.
        subprocess.run(['python3', script_path], check=True,
                       stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)

        # Delete the script upon successful script invocation. We would not get here in the case of
        # a failure.
        os.unlink(script_path)

    def setUp(self):
        '''
        Setup the test.
        '''
        self._start_dir = os.getcwd()

        # Remember the current test ID, so that we can pass it along to child processes when we run
        # functions on different WiredTiger branches.
        self.current_test_id()

        if CompatibilityTestCase._verbose > 2:
            self.prhead(f'Starting the test', True)

        # Set up the test directory.
        test_dir = os.path.join(self._parentTestdir, self.sanitized_shortid())
        shutil.rmtree(test_dir, ignore_errors=True)
        if os.path.exists(test_dir):
            raise Exception(test_dir + ": cannot remove directory")
        os.makedirs(test_dir)

        self._test_dir = os.path.abspath(test_dir)

        os.chdir(self._test_dir)
        self.fdSetUp()

    def tearDown(self):
        '''
        Tear down the test.
        '''
        self.fdTearDown()
        os.chdir(self._start_dir)

def run_tests(suites):
    '''
    Run the test suite(s).
    '''
    from testscenarios.scenarios import generate_scenarios

    result_class = None
    verbose = abstract_test_case.AbstractWiredTigerTestCase._verbose

    if verbose > 1:
        result_class = test_result.PidAwareTextTestResult

    try:
        tests = unittest.TestSuite()
        tests.addTests(generate_scenarios(suites))
        result = unittest.TextTestRunner(verbosity=verbose, resultclass=result_class).run(tests)
        return result
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print("[pid:{}]: ERROR: running test: {}".format(os.getpid(), e))
        raise e

def make_branch_scenarios(older, newer):
    '''
    Create scenarios with the specified older and newer branches.
    '''
    older_branches = []
    for branch in older:
        older_branches.append(('older=%s' % branch, { 'older_branch': branch }))
    newer_branches = []
    for branch in newer:
        newer_branches.append(('newer=%s' % branch, { 'newer_branch': branch }))

    return list(filter(
        lambda scenario: compatibility_common.is_branch_order_asc(scenario[1]['older_branch'],
                                                                  scenario[1]['newer_branch']),
        wtscenario.make_scenarios(older_branches, newer_branches)))

def add_branch_pair_scenarios(suite):
    '''
    Add branch names to the test scenarios.
    '''
    for test in testtools.iterate_tests(suite):
        # Get the older and newer branches, allowing tests to specify their own.
        older = getattr(test, 'older', compatibility_common.BRANCHES)
        newer = getattr(test, 'newer', compatibility_common.BRANCHES)
        if type(older) is not list:
            older = [older]
        if type(newer) is not list:
            newer = [newer]

        # Validate that the test is using only supported branches. Otherwise they may not be ready.
        unsupported_branches = list(filter(lambda b: b not in compatibility_common.BRANCHES,
                                           older + list(set(newer) - set(older))))
        if len(unsupported_branches) > 0:
            unsupported_branches.sort()
            raise Exception('Test \'%s\' specifies unsupported branches: %s'
                            % (test, ', '.join(unsupported_branches)))

        # Make the scenarios from the older and newer branches.
        branch_scenarios = make_branch_scenarios(older, newer)

        # Combine the branch scenarios with the scenarios specified by the test, if any.
        scenarios = getattr(test, 'scenarios', None)
        if scenarios is None:
            setattr(test, 'scenarios', wtscenario.make_scenarios(branch_scenarios))
        else:
            setattr(test, 'scenarios', wtscenario.make_scenarios(branch_scenarios, scenarios))

def global_setup(verbose = 1):
    '''
    Perform the global setup.
    '''

    # Set the default parameters.
    wtscenario.set_long_run(True)

    # Global setup.
    CompatibilityTestCase.setupTestDir()
    CompatibilityTestCase.setupIO(verbose=verbose)
    CompatibilityTestCase.setupRandom()

    # At this point, we can finally set up WiredTiger paths. We need to do this, so that the
    # individual tests can use 'import wiredtiger' at the global scope for simplicity. We are,
    # however, not using the WiredTiger library for any other purpose until the test starts.
    test_util.setup_wiredtiger_path()

def prepare_tests(suites):
    '''
    Prepare the specified test suites for the test.
    '''

    # Find all build configs.
    all_build_configs = [{}]
    for test in testtools.iterate_tests(suites):
        if hasattr(test, 'build_config'):
            config = getattr(test, 'build_config')
            if config is not None and config not in all_build_configs:
                all_build_configs.append(config)

    # Check out and build all relevant branches.
    for branch in compatibility_common.BRANCHES:
        for config in all_build_configs:
            compatibility_common.prepare_branch(branch, config)

    # Add branch arguments to the tests' scenarios.
    add_branch_pair_scenarios(suites)

def run(name='__main__'):
    '''
    Run the specified test.
    '''
    global_setup()

    suite = unittest.TestLoader().loadTestsFromName(name)

    prepare_tests(suite)
    result = run_tests(suite)
    sys.exit(0 if result.wasSuccessful() else 1)

if __name__ == '__main__':
    from discover import defaultTestLoader as loader

    # Parse the command-line arguments
    parser = argparse.ArgumentParser(description='Run the compatibility test suite.')
    parser.add_argument('-v', '--verbose', metavar='N', type=int, choices=range(5), default=1,
                        help='set verboseness to N (0 <= N <= 4, default=1)')
    opts = parser.parse_args()

    # Global setup: set up paths, default parameter values, and test subsystems.
    global_setup(verbose=opts.verbose)

    # Load the tests.
    suite_dir = os.path.dirname(os.path.abspath(__file__))
    suites = loader.discover(suite_dir)

    # Prepare the test suites for execution: check out branches, build them, etc.
    prepare_tests(suites)

    # Run the tests.
    result = run_tests(suites)
    sys.exit(0 if result.wasSuccessful() else 1)
