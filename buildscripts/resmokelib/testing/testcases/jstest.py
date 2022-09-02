"""The unittest.TestCase for JavaScript tests."""
import unittest

import copy
import os
import os.path
import sys
import shutil
import threading

from buildscripts.resmokelib import config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import logging
from buildscripts.resmokelib import utils
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.testcases import interface
from buildscripts.resmokelib.utils import registry


class _SingleJSTestCase(interface.ProcessTestCase):
    """A jstest to execute."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, js_filename, _id, shell_executable=None, shell_options=None):
        """Initialize the _SingleJSTestCase with the JS file to run."""
        interface.ProcessTestCase.__init__(self, logger, "JSTest", js_filename)

        # Command line options override the YAML configuration.
        self.shell_executable = utils.default_if_none(config.MONGO_EXECUTABLE, shell_executable)

        self.js_filename = js_filename
        self._id = _id
        self.shell_options = utils.default_if_none(shell_options, {}).copy()

    def configure(self, fixture, *args, **kwargs):
        """Configure the jstest."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

    def configure_shell(self):
        """Set up the global variables for the shell, and data/ directory for the mongod.

        configure_shell() only needs to be called once per test. Therefore if creating multiple
        _SingleJSTestCase instances to be run in parallel, only call configure_shell() on one of
        them.
        """
        global_vars = self.shell_options.get("global_vars", {}).copy()
        data_dir = self._get_data_dir(global_vars)

        # Set MongoRunner.dataPath if overridden at command line or not specified in YAML.
        if config.DBPATH_PREFIX is not None or "MongoRunner.dataPath" not in global_vars:
            # dataPath property is the dataDir property with a trailing slash.
            data_path = os.path.join(data_dir, "")
        else:
            data_path = global_vars["MongoRunner.dataPath"]

        global_vars["MongoRunner.dataDir"] = data_dir
        global_vars["MongoRunner.dataPath"] = data_path

        test_data = global_vars.get("TestData", {}).copy()
        test_data["minPort"] = core.network.PortAllocator.min_test_port(self.fixture.job_num)
        test_data["maxPort"] = core.network.PortAllocator.max_test_port(self.fixture.job_num)
        test_data["peerPids"] = self.fixture.pids()
        test_data["alwaysUseLogFiles"] = config.ALWAYS_USE_LOG_FILES
        test_data["failIfUnterminatedProcesses"] = True

        # The tests in 'timeseries' directory need to use a different logic for implicity sharding
        # the collection. Make sure that we consider both unix and windows directory structures.
        test_data["implicitlyShardOnCreateCollectionOnly"] = "/timeseries/" in self.js_filename or \
        "\\timeseries\\" in self.js_filename

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

        shutil.rmtree(data_dir, ignore_errors=True)

        try:
            os.makedirs(data_dir)
        except os.error:
            # Directory already exists.
            pass

        process_kwargs = copy.deepcopy(self.shell_options.get("process_kwargs", {}))

        if process_kwargs \
            and "env_vars" in process_kwargs \
            and "KRB5_CONFIG" in process_kwargs["env_vars"] \
            and "KRB5CCNAME" not in process_kwargs["env_vars"]:
            # Use a job-specific credential cache for JavaScript tests involving Kerberos.
            krb5_dir = os.path.join(data_dir, "krb5")

            try:
                os.makedirs(krb5_dir)
            except os.error:
                pass

            process_kwargs["env_vars"]["KRB5CCNAME"] = "DIR:" + krb5_dir

        self.shell_options["process_kwargs"] = process_kwargs

    def _get_data_dir(self, global_vars):
        """Return the value that mongo shell should set for the MongoRunner.dataDir property."""
        # Command line options override the YAML configuration.
        data_dir_prefix = utils.default_if_none(config.DBPATH_PREFIX,
                                                global_vars.get("MongoRunner.dataDir"))
        data_dir_prefix = utils.default_if_none(data_dir_prefix, config.DEFAULT_DBPATH_PREFIX)
        return os.path.join(data_dir_prefix, "job%d" % self.fixture.job_num,
                            config.MONGO_RUNNER_SUBDIR)

    def _make_process(self):
        return core.programs.mongo_shell_program(
            self.logger, executable=self.shell_executable, filename=self.js_filename,
            connection_string=self.fixture.get_driver_connection_url(), **self.shell_options)


class JSTestCaseBuilder:
    """Build the real TestCase in the JSTestCase wrapper."""

    def __init__(self, logger, js_filename, test_id, shell_executable=None, shell_options=None):
        """Initialize the JSTestCase with the JS file to run."""
        self.test_case_template = _SingleJSTestCase(logger, js_filename, test_id, shell_executable,
                                                    shell_options)

    def configure(self, fixture, *args, **kwargs):
        """Configure the jstest."""
        self.test_case_template.configure(fixture, *args, **kwargs)
        self.test_case_template.configure_shell()

    def _make_process(self):
        # This function should only be called by interface.py's as_command().
        return self.test_case_template._make_process()  # pylint: disable=protected-access

    def _get_shell_options_for_thread(self, num_clients, thread_id):
        """Get shell_options with an initialized TestData object for given thread."""

        # We give each _SingleJSTestCase its own copy of the shell_options.
        shell_options = self.test_case_template.shell_options.copy()
        global_vars = shell_options["global_vars"].copy()
        test_data = global_vars["TestData"].copy()

        # We set a property on TestData to mark the main test when multiple clients are going to run
        # concurrently in case there is logic within the test that must execute only once. We also
        # set a property on TestData to indicate how many clients are going to run the test so they
        # can avoid executing certain logic when there may be other operations running concurrently.
        is_main_test = thread_id == 0
        test_data["isMainTest"] = is_main_test
        test_data["numTestClients"] = num_clients

        global_vars["TestData"] = test_data
        shell_options["global_vars"] = global_vars

        return shell_options

    def create_test_case_for_thread(self, logger, num_clients=1, thread_id=0):
        """Create and configure a _SingleJSTestCase to be run in a separate thread."""

        shell_options = self._get_shell_options_for_thread(num_clients, thread_id)
        test_case = _SingleJSTestCase(logger, self.test_case_template.js_filename,
                                      self.test_case_template.id(),
                                      self.test_case_template.shell_executable, shell_options)

        test_case.configure(self.test_case_template.fixture)
        return test_case


class JSTestCase(interface.TestCase, interface.UndoDBUtilsMixin):
    """A wrapper for several copies of a SingleJSTest to execute."""

    REGISTERED_NAME = "js_test"
    TEST_KIND = "JSTest"
    DEFAULT_CLIENT_NUM = 1

    class ThreadWithException(threading.Thread):
        """A wrapper for the thread class that lets us propagate exceptions."""

        def __init__(self, *args, **kwargs):
            """Initialize JSTestCase."""
            threading.Thread.__init__(self, *args, **kwargs)
            self.exc_info = None

        def run(self):
            """Run the jstest."""
            try:
                threading.Thread.run(self)
            except:  # pylint: disable=bare-except
                self.exc_info = sys.exc_info()

    def __init__(self, logger, js_filename, shell_executable=None, shell_options=None):
        """Initialize the TestCase for running JS files."""
        super().__init__(logger, self.TEST_KIND, js_filename)
        self.num_clients = JSTestCase.DEFAULT_CLIENT_NUM
        self._builder = JSTestCaseBuilder(logger, js_filename, self.id(), shell_executable,
                                          shell_options)

    def configure(  # pylint: disable=arguments-differ,keyword-arg-before-vararg
            self, fixture, num_clients=DEFAULT_CLIENT_NUM, *args, **kwargs):
        """Configure the jstest."""
        super().configure(fixture, *args, **kwargs)
        self.num_clients = num_clients
        self._builder.configure(fixture, *args, **kwargs)

    def _make_process(self):
        # This function should only be called by interface.py's as_command().
        return self._builder._make_process()  # pylint: disable=protected-access

    def _run_single_copy(self):
        test_case = self._builder.create_test_case_for_thread(self.logger)
        try:
            test_case.run_test()
            # If there was an exception, it will be logged in test_case's run_test function.
        finally:
            self.return_code = test_case.return_code
            self._raise_if_unsafe_exit(self.return_code)

    def _run_multiple_copies(self):
        threads = []
        test_cases = []
        try:
            # If there are multiple clients, make a new thread for each client.
            for thread_id in range(self.num_clients):
                logger = logging.loggers.new_test_thread_logger(self.logger, self.test_kind,
                                                                str(thread_id))
                test_case = self._builder.create_test_case_for_thread(
                    logger, num_clients=self.num_clients, thread_id=thread_id)
                test_cases.append(test_case)

                thread = self.ThreadWithException(target=test_case.run_test)
                threads.append(thread)
                thread.start()
        except:
            self.logger.exception("Encountered an error starting threads for jstest %s.",
                                  self.basename())
            raise
        finally:
            for thread in threads:
                thread.join()

            # Go through each test's return codes, asserting safe exits and storing the last nonzero code.
            return_code = 0
            for test_case in test_cases:
                if test_case.return_code != 0:
                    self._raise_if_unsafe_exit(return_code)
                    return_code = test_case.return_code
            self.return_code = return_code

            for (thread_id, thread) in enumerate(threads):
                if thread.exc_info is not None:
                    if not isinstance(thread.exc_info[1], self.failureException):
                        self.logger.error(
                            "Encountered an error inside thread %d running jstest %s.", thread_id,
                            self.basename(), exc_info=thread.exc_info)
                    raise thread.exc_info[1]

    def run_test(self):
        """Execute the test."""
        try:
            if self.num_clients == 1:
                self._run_single_copy()
            else:
                self._run_multiple_copies()
        except:
            # Archive any available recordings if there's any failure. It's possible a problem
            # with the recorder will cause no recordings to be generated. There will also be
            # recordings of other processes, we keep them to avoid complicating this code.
            self._cull_recordings("mongo")
            raise

    def _raise_if_unsafe_exit(self, return_code):
        """Determine if a return code represents and unsafe exit."""
        # 252 and 253 may be returned in failed test executions.
        # (i.e. -4 and -3 in mongo_main.cpp)
        if return_code not in (252, 253, 0):
            self.propagate_error = errors.UnsafeExitError(
                f"Mongo shell exited with code {return_code} while running jstest {self.basename()}."
                " Further test execution may be unsafe.")
            raise self.propagate_error  # pylint: disable=raising-bad-type


class AllVersionsJSTestCase(JSTestCase):
    """
    Alias for JSTestCase for multiversion passthrough suites.

    It run with all combinations of versions of replica sets and sharded clusters.
    The distinct name is picked up by task generation.
    """

    REGISTERED_NAME = "all_versions_js_test"
