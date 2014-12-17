"""
Testers - TestCase wrappers for tests of different types
"""

import unittest

from external_programs import *
from mongodb_programs import DBTest
from mongodb_programs import MongoShell
from mongodb_programs import MongoShellContext


DEFAULT_TESTER_CLASSES = {"js_test": "smoke.JSUnitTest",
                          "db_test": "smoke.DBTestUnitTest",
                          "exe_test": "smoke.ExeUnitTest"}


class JSUnitTest(unittest.TestCase):

    """A MongoDB shell 'jstest' wrapped as a TestCase.

    Allows fixtures to provide global variables and databases to connect to as API additions.

    """

    def __init__(self, jstest, test_apis, logger, shell_executable="./mongo", shell_options={},
                 *args, **kwargs):

        unittest.TestCase.__init__(self, *args, **kwargs)

        # Setup the description for the unit test
        self._testMethodDoc = "JSTest %s" % jstest.filename

        self.jstest = jstest
        self.test_apis = test_apis
        self.logger = logger

        self.shell_executable = shell_executable
        self.shell_options = {}
        self.shell_options.update(shell_options)
        
        self.shell_context = MongoShellContext()

    def setUp(self):
        try:
            for api in self.test_apis:
                api.add_to_shell(self.shell_context)
        except:
            self.logger.error("Setup failed for shell API.", exc_info=True)
            raise

    def runTest(self):

        shell = MongoShell(executable=self.shell_executable,
                           shell_context=self.shell_context,
                           js_filenames=[self.jstest.filename],
                           context=ExternalContext(logger=self.logger),
                           **self.shell_options)

        try:
            self.logger.info("Starting MongoDB shell...\n%s" % shell)

            shell.start()

            self.logger.info("MongoDB shell started with pid %s." % shell.pid())

            return_code = shell.wait()
            if return_code != 0:
                raise Exception("JSTest %s failed." % self.jstest.filename)

            self.logger.info("MongoDB shell finished.")

        except:
            self.logger.error("MongoDB shell failed.", exc_info=True)
            raise

    def tearDown(self):
        try:
            for api in self.test_apis:
                api.teardown_api()
        except:
            self.logger.error("Teardown failed for shell API.", exc_info=True)
            raise


class ExeUnitTest(unittest.TestCase):

    """An arbitrary executable file wrapped as a TestCase.

    Meant for use with C++ unit tests, for example.

    Allows fixtures to provide environment variables as API additions.

    """

    def __init__(self, exetest, test_apis, logger,
                 program_options={},
                 *args, **kwargs):

        unittest.TestCase.__init__(self, *args, **kwargs)
        self.exetest = exetest
        self.test_apis = test_apis
        self.logger = logger

        # Setup the description for the unit test
        self._testMethodDoc = "Program %s" % self.exetest.filename

        self.process_context = ExternalContext(logger=self.logger)
        if program_options:
            self.process_context.kwargs.update(program_options)

    def setUp(self):
        try:
            for api in self.test_apis:
                api.add_to_process(self.process_context)
        except:
            self.logger.error("Setup failed for process API.", exc_info=True)
            raise

    def runTest(self):

        program = ExternalProgram(executable=self.exetest.filename,
                                  context=self.process_context)

        try:
            self.logger.info("Starting Program...\n%s" % program)

            program.start()

            self.logger.info("Program %s started with pid %s." %
                             (self.exetest.filename, program.pid()))

            return_code = program.wait()
            if return_code != 0:
                raise Exception("Program %s failed." % self.exetest.filename)

            self.logger.info("Program finished.")

        except:
            self.logger.error("Program failed.", exc_info=True)
            raise

    def tearDown(self):
        try:
            for api in self.test_apis:
                api.teardown_api()
        except:
            self.log.error("Teardown failed for process API.", exc_info=True)
            raise


class DBTestUnitTest(ExeUnitTest):

    """A executable MongoDB 'dbtest' wrapped as a TestCase.

    Individual dbtests can be specified optionally.

    Allows fixtures to provide environment variables as API additions.

    """

    def __init__(self, dbtest, test_apis, logger,
                 dbtest_executable=None,
                 dbtest_options={},
                 *args, **kwargs):

        ExeUnitTest.__init__(self, dbtest, test_apis, logger, dbtest_options,
                             *args, **kwargs)
        self.dbtest = dbtest

        self.dbtest_names = []
        if "dbtest_names" in dbtest.metadata:
            self.dbtest_names = dbtest.metadata["dbtest_names"]

        # Setup the description for the unit test
        self._testMethodDoc = "DBTest %s" % (" ".join(self.dbtest_names))

        self.dbtest_executable = dbtest_executable

    def runTest(self):

        dbtest = DBTest(executable=self.dbtest_executable,
                        dbtest_names=self.dbtest_names,
                        context=self.process_context)
        try:
            self.logger.info("Starting DBTest...\n%s" % dbtest)

            dbtest.start()

            self.logger.info("DBTest %s started with pid %s." % (" ".join(self.dbtest_names),
                                                                 dbtest.pid()))

            return_code = dbtest.wait()
            if return_code != 0:
                raise Exception("DBTest %s failed." % (" ".join(self.dbtest_names)))

            self.logger.info("DBTest finished.")

        except:
            self.logger.error("DBTest failed.", exc_info=True)
            raise
