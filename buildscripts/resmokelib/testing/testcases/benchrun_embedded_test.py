"""The unittest.TestCase for tests using benchrun embedded (mongoebench)."""

from __future__ import absolute_import

import os

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import parser
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import interface


class BenchrunEmbeddedTestCase(interface.ProcessTestCase):
    """A Benchrun embedded test to execute."""

    REGISTERED_NAME = "benchrun_embedded_test"

    def __init__(self, logger, mongoebench_config_file, program_options=None):
        """Initialize the BenchrunEmbeddedTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "Benchmark embedded test",
                                           mongoebench_config_file)
        parser.validate_benchmark_options()

        self.benchrun_config_file = mongoebench_config_file

        # Command line options override the YAML configuration.
        self.benchrun_executable = utils.default_if_none(_config.MONGOEBENCH_EXECUTABLE,
                                                         _config.DEFAULT_MONGOEBENCH_EXECUTABLE)
        self.benchrun_repetitions = utils.default_if_none(_config.BENCHMARK_REPETITIONS,
                                                          _config.DEFAULT_BENCHMARK_REPETITIONS)
        self.suite_benchrun_options = program_options
        self.benchrun_threads = 1
        if program_options and "threads" in program_options:
            self.benchrun_threads = program_options["threads"]
        self.benchrun_options = {}

        # Set the dbpath.
        dbpath = utils.default_if_none(_config.DBPATH_PREFIX, _config.DEFAULT_DBPATH_PREFIX)
        self.dbpath = os.path.join(dbpath, "mongoebench")

    def configure(self, fixture, *args, **kwargs):
        """Configure BenchrunEmbeddedTestCase."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        # 1. Set the default benchmark options.
        benchrun_options = {"time": _config.DEFAULT_BENCHMARK_MIN_TIME.total_seconds()}

        # 2. Override Benchmark options with options set through `program_options` in the suite
        #    configuration.
        suite_benchrun_options = utils.default_if_none(self.suite_benchrun_options, {})
        benchrun_options.update(suite_benchrun_options)

        # 3. Override Benchmark options with options set through resmoke's command line.
        resmoke_benchrun_options = {"dbpath": self.dbpath, "time": _config.BENCHMARK_MIN_TIME}

        for key, value in resmoke_benchrun_options.items():
            if value is not None:
                # 4. sanitize options before passing them to Benchmark's command line.
                if key == "time":
                    value = value.total_seconds()
                benchrun_options[key] = value

        self.benchrun_options = benchrun_options

        # Create the dbpath.
        self._clear_dbpath()
        try:
            os.makedirs(self.dbpath)
        except os.error:
            # Directory already exists.
            pass

    def run_test(self):
        """Run the test for specified number of iterations."""
        for it_num in xrange(self.benchrun_repetitions):
            # Set the output file for each iteration.
            self.benchrun_options["output"] = self._report_name(it_num)
            interface.ProcessTestCase.run_test(self)

    def _clear_dbpath(self):
        utils.rmtree(self.dbpath, ignore_errors=True)

    def _report_name(self, iter_num):
        """Return the constructed report name.

        The report name is of the form mongoebench.<test_name>.<num threads>.<iteration num>.json.
        """
        return "mongoebench.{}.{}.{}.json".format(self.short_name(), self.benchrun_threads,
                                                  iter_num)

    def _make_process(self):
        return core.programs.generic_program(self.logger,
                                             [self.benchrun_executable, self.benchrun_config_file],
                                             **self.benchrun_options)
