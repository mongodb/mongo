"""The unittest.TestCase for JavaScript tests."""

from __future__ import absolute_import

import os
import os.path
import sys
import threading

from . import interface
from ... import config
from ... import core
from ... import utils
from ...utils import registry


class _SingleJSTestCase(interface.ProcessTestCase):
    """A jstest to execute."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, js_filename, shell_executable=None, shell_options=None):
        """Initialize the _SingleJSTestCase with the JS file to run."""

        interface.ProcessTestCase.__init__(self, logger, "JSTest", js_filename)

        # Command line options override the YAML configuration.
        self.shell_executable = utils.default_if_none(config.MONGO_EXECUTABLE, shell_executable)

        self.js_filename = js_filename
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

        # Don't set the path to the executables when the user didn't specify them via the command
        # line. The functions in the mongo shell for spawning processes have their own logic for
        # determining the default path to use.
        if config.MONGOD_EXECUTABLE is not None:
            global_vars["MongoRunner.mongodPath"] = config.MONGOD_EXECUTABLE
        if config.MONGOS_EXECUTABLE is not None:
            global_vars["MongoRunner.mongosPath"] = config.MONGOS_EXECUTABLE
        if self.shell_executable is not None:
            global_vars["MongoRunner.mongoShellPath"] = self.shell_executable

        test_data = global_vars.get("TestData", {}).copy()
        test_data["minPort"] = core.network.PortAllocator.min_test_port(self.fixture.job_num)
        test_data["maxPort"] = core.network.PortAllocator.max_test_port(self.fixture.job_num)
        test_data["failIfUnterminatedProcesses"] = True

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

        utils.rmtree(data_dir, ignore_errors=True)

        try:
            os.makedirs(data_dir)
        except os.error:
            # Directory already exists.
            pass

        process_kwargs = self.shell_options.get("process_kwargs", {}).copy()

        if "KRB5_CONFIG" in process_kwargs and "KRB5CCNAME" not in process_kwargs:
            # Use a job-specific credential cache for JavaScript tests involving Kerberos.
            krb5_dir = os.path.join(data_dir, "krb5")
            try:
                os.makedirs(krb5_dir)
            except os.error:
                pass
            process_kwargs["KRB5CCNAME"] = "DIR:" + os.path.join(krb5_dir, ".")

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


class JSTestCase(interface.ProcessTestCase):
    """A wrapper for several copies of a SingleJSTest to execute."""

    REGISTERED_NAME = "js_test"

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

    DEFAULT_CLIENT_NUM = 1

    def __init__(self, logger, js_filename, shell_executable=None, shell_options=None):
        """Initialize the JSTestCase with the JS file to run."""

        interface.ProcessTestCase.__init__(self, logger, "JSTest", js_filename)
        self.num_clients = JSTestCase.DEFAULT_CLIENT_NUM
        self.test_case_template = _SingleJSTestCase(logger, js_filename, shell_executable,
                                                    shell_options)

    def configure(  # pylint: disable=arguments-differ,keyword-arg-before-vararg
            self, fixture, num_clients=DEFAULT_CLIENT_NUM, *args, **kwargs):
        """Configure the jstest."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)
        self.num_clients = num_clients
        self.test_case_template.configure(fixture, *args, **kwargs)
        self.test_case_template.configure_shell()

    def _make_process(self):
        # This function should only be called by interface.py's as_command().
        return self.test_case_template._make_process()  # pylint: disable=protected-access

    def _get_shell_options_for_thread(self, thread_id):
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
        test_data["numTestClients"] = self.num_clients

        global_vars["TestData"] = test_data
        shell_options["global_vars"] = global_vars

        return shell_options

    def _create_test_case_for_thread(self, logger, thread_id):
        """Create and configure a _SingleJSTestCase to be run in a separate thread."""

        shell_options = self._get_shell_options_for_thread(thread_id)
        test_case = _SingleJSTestCase(logger, self.test_case_template.js_filename,
                                      self.test_case_template.shell_executable, shell_options)

        test_case.configure(self.fixture)
        return test_case

    def _run_single_copy(self):
        test_case = self._create_test_case_for_thread(self.logger, thread_id=0)
        try:
            test_case.run_test()
            # If there was an exception, it will be logged in test_case's run_test function.
        finally:
            self.return_code = test_case.return_code

    def _run_multiple_copies(self):
        threads = []
        test_cases = []
        try:
            # If there are multiple clients, make a new thread for each client.
            for thread_id in xrange(self.num_clients):
                logger = self.logger.new_test_thread_logger(self.test_kind, str(thread_id))
                test_case = self._create_test_case_for_thread(logger, thread_id)
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

            # Go through each test's return code and store the first nonzero one if it exists.
            return_code = 0
            for test_case in test_cases:
                if test_case.return_code != 0:
                    return_code = test_case.return_code
                    break
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
        if self.num_clients == 1:
            self._run_single_copy()
        else:
            self._run_multiple_copies()
