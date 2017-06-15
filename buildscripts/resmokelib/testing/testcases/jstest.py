"""
unittest.TestCase for JavaScript tests.
"""

from __future__ import absolute_import

import os
import os.path
import shutil
import sys
import threading

from . import interface
from ... import config
from ... import core
from ... import utils


class JSTestCase(interface.TestCase):
    """
    A jstest to execute.
    """

    REGISTERED_NAME = "js_test"

    # A wrapper for the thread class that lets us propagate exceptions.
    class ExceptionThread(threading.Thread):
        def __init__(self, my_target, my_args):
            threading.Thread.__init__(self, target=my_target, args=my_args)
            self.err = None

        def run(self):
            try:
                threading.Thread.run(self)
            except:
                self.err = sys.exc_info()[1]
            else:
                self.err = None

        def _get_exception(self):
            return self.err

    DEFAULT_CLIENT_NUM = 1

    def __init__(self,
                 logger,
                 js_filename,
                 shell_executable=None,
                 shell_options=None,
                 test_kind="JSTest"):
        """Initializes the JSTestCase with the JS file to run."""

        interface.TestCase.__init__(self, logger, test_kind, js_filename)

        # Command line options override the YAML configuration.
        self.shell_executable = utils.default_if_none(config.MONGO_EXECUTABLE, shell_executable)

        self.js_filename = js_filename
        self.shell_options = utils.default_if_none(shell_options, {}).copy()
        self.num_clients = JSTestCase.DEFAULT_CLIENT_NUM

    def configure(self, fixture, num_clients=DEFAULT_CLIENT_NUM, *args, **kwargs):
        interface.TestCase.configure(self, fixture, *args, **kwargs)

        if self.fixture.port is not None:
            self.shell_options["port"] = self.fixture.port

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
        test_data["minPort"] = core.network.PortAllocator.min_test_port(fixture.job_num)
        test_data["maxPort"] = core.network.PortAllocator.max_test_port(fixture.job_num)

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

        shutil.rmtree(data_dir, ignore_errors=True)

        self.num_clients = num_clients

        try:
            os.makedirs(data_dir)
        except os.error:
            # Directory already exists.
            pass

    def _get_data_dir(self, global_vars):
        """
        Returns the value that the mongo shell should set for the
        MongoRunner.dataDir property.
        """

        # Command line options override the YAML configuration.
        data_dir_prefix = utils.default_if_none(config.DBPATH_PREFIX,
                                                global_vars.get("MongoRunner.dataDir"))
        data_dir_prefix = utils.default_if_none(data_dir_prefix, config.DEFAULT_DBPATH_PREFIX)
        return os.path.join(data_dir_prefix,
                            "job%d" % (self.fixture.job_num),
                            config.MONGO_RUNNER_SUBDIR)

    def run_test(self):
        threads = []
        try:
            # Don't thread if there is only one client.
            if self.num_clients == 1:
                shell = self._make_process(self.logger)
                self._execute(shell)
            else:
                # If there are multiple clients, make a new thread for each client.
                for i in xrange(self.num_clients):
                    t = self.ExceptionThread(my_target=self._run_test_in_thread, my_args=[i])
                    t.start()
                    threads.append(t)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running jstest %s.", self.basename())
            raise
        finally:
            for t in threads:
                t.join()
            for t in threads:
                if t._get_exception() is not None:
                    raise t._get_exception()

    def _make_process(self, logger=None, thread_id=0):
        # Since _make_process() is called by each thread, we make a shallow copy of the mongo shell
        # options to avoid modifying the shared options for the JSTestCase.
        shell_options = self.shell_options.copy()
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

        # If logger is none, it means that it's not running in a thread and thus logger should be
        # set to self.logger.
        logger = utils.default_if_none(logger, self.logger)

        return core.programs.mongo_shell_program(logger,
                                                 executable=self.shell_executable,
                                                 filename=self.js_filename,
                                                 **shell_options)

    def _run_test_in_thread(self, thread_id):
        # Make a logger for each thread. When this method gets called self.logger has been
        # overridden with a TestLogger instance by the TestReport in the startTest() method.
        logger = self.logger.new_test_thread_logger(self.test_kind, str(thread_id))
        shell = self._make_process(logger, thread_id)
        self._execute(shell)
