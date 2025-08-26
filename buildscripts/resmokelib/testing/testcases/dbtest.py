"""The unittest.TestCase for dbtests."""

import copy
import os
import os.path
import shutil
from typing import Optional

from buildscripts.resmokelib import config, core, logging, utils
from buildscripts.resmokelib.testing.testcases import interface


class DBTestCase(interface.ProcessTestCase):
    """A dbtest to execute."""

    REGISTERED_NAME = "db_test"

    def __init__(
        self,
        logger: logging.Logger,
        dbtest_suites: list[str],
        dbtest_executable: Optional[str] = None,
        dbtest_options: Optional[dict] = None,
    ):
        """Initialize the DBTestCase with the dbtest suite to run."""

        assert len(dbtest_suites) == 1
        interface.ProcessTestCase.__init__(self, logger, "dbtest suite", dbtest_suites[0])

        # Command line options override the YAML configuration.
        self.dbtest_executable = utils.default_if_none(config.DBTEST_EXECUTABLE, dbtest_executable)

        self.dbtest_suite = dbtest_suites[0]
        self.dbtest_options = utils.default_if_none(dbtest_options, {}).copy()

    def configure(self, fixture, *args, **kwargs):
        """Configure DBTestCase."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        # If a dbpath was specified, then use it as a container for all other dbpaths.
        dbpath_prefix = self.dbtest_options.pop("dbpath", DBTestCase._get_dbpath_prefix())
        dbpath = os.path.join(dbpath_prefix, "job%d" % self.fixture.job_num, "unittest")
        self.dbtest_options["dbpath"] = dbpath

        self._clear_dbpath()

        try:
            os.makedirs(dbpath)
        except os.error:
            # Directory already exists.
            pass

        process_kwargs = copy.deepcopy(self.dbtest_options.get("process_kwargs", {}))
        interface.append_process_tracking_options(process_kwargs, self._id)
        self.dbtest_options["process_kwargs"] = process_kwargs


    def _execute(self, process):
        interface.ProcessTestCase._execute(self, process)
        self._clear_dbpath()

    def _clear_dbpath(self):
        shutil.rmtree(self.dbtest_options["dbpath"], ignore_errors=True)

    def _make_process(self):
        return core.programs.dbtest_program(
            self.logger,
            executable=self.dbtest_executable,
            suites=[self.dbtest_suite],
            **self.dbtest_options,
        )

    @staticmethod
    def _get_dbpath_prefix():
        """
        Return the prefix of the dbpath to use for the dbtest executable.

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
