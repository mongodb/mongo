"""
unittest.TestCase for dbtests.
"""

from __future__ import absolute_import

import os
import os.path
import shutil

from . import interface
from ... import config
from ... import core
from ... import utils


class DBTestCase(interface.TestCase):
    """
    A dbtest to execute.
    """

    REGISTERED_NAME = "db_test"

    def __init__(self,
                 logger,
                 dbtest_suite,
                 dbtest_executable=None,
                 dbtest_options=None):
        """
        Initializes the DBTestCase with the dbtest suite to run.
        """

        interface.TestCase.__init__(self, logger, "DBTest", dbtest_suite)

        # Command line options override the YAML configuration.
        self.dbtest_executable = utils.default_if_none(config.DBTEST_EXECUTABLE, dbtest_executable)

        self.dbtest_suite = dbtest_suite
        self.dbtest_options = utils.default_if_none(dbtest_options, {}).copy()

    def configure(self, fixture, *args, **kwargs):
        interface.TestCase.configure(self, fixture, *args, **kwargs)

        # If a dbpath was specified, then use it as a container for all other dbpaths.
        dbpath_prefix = self.dbtest_options.pop("dbpath", DBTestCase._get_dbpath_prefix())
        dbpath = os.path.join(dbpath_prefix, "job%d" % (self.fixture.job_num), "unittest")
        self.dbtest_options["dbpath"] = dbpath

        shutil.rmtree(dbpath, ignore_errors=True)

        try:
            os.makedirs(dbpath)
        except os.error:
            # Directory already exists.
            pass

    def run_test(self):
        try:
            dbtest = self._make_process()
            self._execute(dbtest)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running dbtest suite %s.", self.basename())
            raise

    def _make_process(self):
        return core.programs.dbtest_program(self.logger,
                                            executable=self.dbtest_executable,
                                            suites=[self.dbtest_suite],
                                            **self.dbtest_options)

    @staticmethod
    def _get_dbpath_prefix():
        """
        Returns the prefix of the dbpath to use for the dbtest
        executable.

        Order of preference:
          1. The --dbpathPrefix specified at the command line.
          2. Value of the TMPDIR environment variable.
          3. Value of the TEMP environment variable.
          4. Value of the TMP environment variable.
          5. The /tmp directory.
        """

        if config.DBPATH_PREFIX is not None:
            return config.DBPATH_PREFIX

        for env_var in ("TMPDIR", "TEMP", "TMP"):
            if env_var in os.environ:
                return os.environ[env_var]
        return os.path.normpath("/tmp")
