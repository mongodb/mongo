#!/usr/bin/env python3
"""Command line utility for executing MongoDB tests of all kinds."""

import os.path
import platform
import random
import subprocess
import sys
import tarfile
import time

import pkg_resources
import requests

try:
    import grpc_tools.protoc
    import grpc
except ImportError:
    pass

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.resmokelib import config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import logging
from buildscripts.resmokelib import parser
from buildscripts.resmokelib import reportfile
from buildscripts.resmokelib import sighandler
from buildscripts.resmokelib import suitesconfig
from buildscripts.resmokelib import testing
from buildscripts.resmokelib import utils

from buildscripts.resmokelib.core import process
from buildscripts.resmokelib.core import jasper_process


class Resmoke(object):  # pylint: disable=too-many-instance-attributes
    """The main class to run tests with resmoke."""

    def __init__(self):
        """Initialize the Resmoke instance."""
        self.__start_time = time.time()
        self._config = None
        self._exec_logger = None
        self._resmoke_logger = None
        self._archive = None
        self._jasper_server = None
        self._interrupted = False
        self._exit_code = 0

    def configure_from_command_line(self):
        """Configure this instance using the command line arguments."""
        self._config = parser.parse_command_line()

    def _setup_logging(self):
        logging.loggers.configure_loggers(self._config.logging_config)
        logging.flush.start_thread()
        self._exec_logger = logging.loggers.EXECUTOR_LOGGER
        self._resmoke_logger = self._exec_logger.new_resmoke_logger()

    def _exit_logging(self):
        if not self._interrupted:
            logging.flush.stop_thread()
            self._exit_on_incomplete_logging()

    def _exit_on_incomplete_logging(self):
        if logging.buildlogger.is_log_output_incomplete():
            if self._exit_code == 0:
                # We don't anticipate users to look at passing Evergreen tasks very often that even
                # if the log output is incomplete, we'd still rather not show anything in the
                # Evergreen UI or cause a JIRA ticket to be created.
                self._resmoke_logger.info(
                    "We failed to flush all log output to logkeeper but all tests passed, so"
                    " ignoring.")
                return

            exit_code = errors.LoggerRuntimeConfigError.EXIT_CODE
            self._resmoke_logger.info(
                "Exiting with code %d rather than requested code %d because we failed to flush all"
                " log output to logkeeper.", exit_code, self._exit_code)
            self.exit(exit_code)

    def run(self):
        """Run resmoke."""
        if self._config is None:
            raise RuntimeError("Resmoke must be configured before calling run()")
        self._setup_logging()

        try:
            if self._config.list_suites:
                self.list_suites()
            elif self._config.find_suites:
                self.find_suites()
            elif self._config.dry_run == "tests":
                self.dry_run()
            else:
                self.run_tests()
        finally:
            self._exit_logging()

    def list_suites(self):
        """List the suites that are available to execute."""
        suite_names = suitesconfig.get_named_suites()
        self._resmoke_logger.info("Suites available to execute:\n%s", "\n".join(suite_names))

    def find_suites(self):
        """List the suites that run the specified tests."""
        suites = self._get_suites()
        suites_by_test = self._find_suites_by_test(suites)
        for test in sorted(suites_by_test):
            suite_names = suites_by_test[test]
            self._resmoke_logger.info("%s will be run by the following suite(s): %s", test,
                                      suite_names)

    @staticmethod
    def _find_suites_by_test(suites):
        """
        Look up what other resmoke suites run the tests specified in the suites parameter.

        Return a dict keyed by test name, value is array of suite names.
        """
        memberships = {}
        test_membership = suitesconfig.create_test_membership_map()
        for suite in suites:
            for test in suite.tests:
                memberships[test] = test_membership[test]
        return memberships

    def dry_run(self):
        """List which tests would run and which tests would be excluded in a resmoke invocation."""
        suites = self._get_suites()
        for suite in suites:
            self._shuffle_tests(suite)
            sb = ["Tests that would be run in suite {}".format(suite.get_display_name())]
            sb.extend(suite.tests or ["(no tests)"])
            sb.append("Tests that would be excluded from suite {}".format(suite.get_display_name()))
            sb.extend(suite.excluded or ["(no tests)"])
            self._exec_logger.info("\n".join(sb))

    def run_tests(self):
        """Run the suite and tests specified."""
        self._resmoke_logger.info("verbatim resmoke.py invocation: %s", " ".join(sys.argv))

        if config.EVERGREEN_TASK_ID:
            local_args = parser.to_local_args()
            self._resmoke_logger.info("resmoke.py invocation for local usage: %s %s",
                                      os.path.join("buildscripts", "resmoke.py"),
                                      " ".join(local_args))

        suites = None
        try:
            suites = self._get_suites()
            self._setup_archival()
            if config.SPAWN_USING == "jasper":
                self._setup_jasper()
            self._setup_signal_handler(suites)

            for suite in suites:
                self._interrupted = self._run_suite(suite)
                if self._interrupted or (suite.options.fail_fast and suite.return_code != 0):
                    self._log_resmoke_summary(suites)
                    self.exit(suite.return_code)

            self._log_resmoke_summary(suites)

            # Exit with a nonzero code if any of the suites failed.
            exit_code = max(suite.return_code for suite in suites)
            self.exit(exit_code)
        finally:
            if config.SPAWN_USING == "jasper":
                self._exit_jasper()
            self._exit_archival()
            if suites:
                reportfile.write(suites)

    def _run_suite(self, suite):
        """Run a test suite."""
        self._log_suite_config(suite)
        suite.record_suite_start()
        interrupted = self._execute_suite(suite)
        suite.record_suite_end()
        self._log_suite_summary(suite)
        return interrupted

    def _log_resmoke_summary(self, suites):
        """Log a summary of the resmoke run."""
        time_taken = time.time() - self.__start_time
        if len(self._config.suite_files) > 1:
            testing.suite.Suite.log_summaries(self._resmoke_logger, suites, time_taken)

    def _log_suite_summary(self, suite):
        """Log a summary of the suite run."""
        self._resmoke_logger.info("=" * 80)
        self._resmoke_logger.info("Summary of %s suite: %s", suite.get_display_name(),
                                  self._get_suite_summary(suite))

    def _execute_suite(self, suite):
        """Execute a suite and return True if interrupted, False otherwise."""
        self._shuffle_tests(suite)
        if not suite.tests:
            self._exec_logger.info("Skipping %s, no tests to run", suite.test_kind)
            suite.return_code = 0
            return False
        executor_config = suite.get_executor_config()
        try:
            executor = testing.executor.TestSuiteExecutor(
                self._exec_logger, suite, archive_instance=self._archive, **executor_config)
            executor.run()
        except (errors.UserInterrupt, errors.LoggerRuntimeConfigError) as err:
            self._exec_logger.error("Encountered an error when running %ss of suite %s: %s",
                                    suite.test_kind, suite.get_display_name(), err)
            suite.return_code = err.EXIT_CODE
            return True
        except IOError:
            suite.return_code = 74  # Exit code for IOError on POSIX systems.
            return True
        except:  # pylint: disable=bare-except
            self._exec_logger.exception("Encountered an error when running %ss of suite %s.",
                                        suite.test_kind, suite.get_display_name())
            suite.return_code = 2
            return False
        return False

    def _shuffle_tests(self, suite):
        """Shuffle the tests if the shuffle cli option was set."""
        random.seed(config.RANDOM_SEED)
        if not config.SHUFFLE:
            return
        self._exec_logger.info("Shuffling order of tests for %ss in suite %s. The seed is %d.",
                               suite.test_kind, suite.get_display_name(), config.RANDOM_SEED)
        random.shuffle(suite.tests)

    def _get_suites(self):
        """Return the list of suites for this resmoke invocation."""
        try:
            return suitesconfig.get_suites(self._config.suite_files, self._config.test_files)
        except errors.SuiteNotFound as err:
            self._resmoke_logger.error("Failed to parse YAML suite definition: %s", str(err))
            self.list_suites()
            self.exit(1)

    def _log_suite_config(self, suite):
        sb = [
            "YAML configuration of suite {}".format(suite.get_display_name()),
            utils.dump_yaml({"test_kind": suite.get_test_kind_config()}), "",
            utils.dump_yaml({"selector": suite.get_selector_config()}), "",
            utils.dump_yaml({"executor": suite.get_executor_config()}), "",
            utils.dump_yaml({"logging": self._config.logging_config})
        ]
        self._resmoke_logger.info("\n".join(sb))

    @staticmethod
    def _get_suite_summary(suite):
        """Return a summary of the suite run."""
        sb = []
        suite.summarize(sb)
        return "\n".join(sb)

    def _setup_signal_handler(self, suites):
        """Set up a SIGUSR1 signal handler that logs a test result summary and a thread dump."""
        sighandler.register(self._resmoke_logger, suites, self.__start_time)

    def _setup_archival(self):
        """Set up the archival feature if enabled in the cli options."""
        if config.ARCHIVE_FILE:
            self._archive = utils.archival.Archival(
                archival_json_file=config.ARCHIVE_FILE, limit_size_mb=config.ARCHIVE_LIMIT_MB,
                limit_files=config.ARCHIVE_LIMIT_TESTS, logger=self._exec_logger)

    def _exit_archival(self):
        """Finish up archival tasks before exit if enabled in the cli options."""
        if self._archive and not self._interrupted:
            self._archive.exit()

    # pylint: disable=too-many-instance-attributes,too-many-statements,too-many-locals
    def _setup_jasper(self):
        """Start up the jasper process manager."""
        root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        proto_file = os.path.join(root_dir, "buildscripts", "resmokelib", "core", "jasper.proto")
        try:
            well_known_protos_include = pkg_resources.resource_filename("grpc_tools", "_proto")
        except ImportError:
            raise ImportError("You must run: sys.executable + '-m pip install grpcio grpcio-tools "
                              "googleapis-common-protos' to use --spawnUsing=jasper.")

        # We use the build/ directory as the output directory because the generated files aren't
        # meant to because tracked by git or linted.
        proto_out = os.path.join(root_dir, "build", "jasper")

        utils.rmtree(proto_out, ignore_errors=True)
        os.makedirs(proto_out)

        # We make 'proto_out' into a Python package so we can add it to 'sys.path' and import the
        # *pb2*.py modules from it.
        with open(os.path.join(proto_out, "__init__.py"), "w"):
            pass

        ret = grpc_tools.protoc.main([
            grpc_tools.protoc.__file__,
            "--grpc_python_out",
            proto_out,
            "--python_out",
            proto_out,
            "--proto_path",
            os.path.dirname(proto_file),
            "--proto_path",
            well_known_protos_include,
            os.path.basename(proto_file),
        ])

        if ret != 0:
            raise RuntimeError("Failed to generated gRPC files from the jasper.proto file")

        sys.path.append(os.path.dirname(proto_out))

        from jasper import jasper_pb2
        from jasper import jasper_pb2_grpc

        jasper_process.Process.jasper_pb2 = jasper_pb2
        jasper_process.Process.jasper_pb2_grpc = jasper_pb2_grpc

        curator_path = "build/curator"
        if sys.platform == "win32":
            curator_path += ".exe"
        git_hash = "d846f0c875716e9377044ab2a50542724369662a"
        curator_exists = os.path.isfile(curator_path)
        curator_same_version = False
        if curator_exists:
            curator_version = subprocess.check_output([curator_path,
                                                       "--version"]).decode('utf-8').split()
            curator_same_version = git_hash in curator_version

        if curator_exists and not curator_same_version:
            os.remove(curator_path)
            self._resmoke_logger.info(
                "Found a different version of curator. Downloading version %s of curator to enable"
                "process management using jasper.", git_hash)

        if not curator_exists or not curator_same_version:
            if sys.platform == "darwin":
                os_platform = "macos"
            elif sys.platform == "win32":
                os_platform = "windows-64"
            elif sys.platform.startswith("linux"):
                os_platform = "ubuntu1604"
            else:
                raise OSError("Unrecognized platform. "
                              "This program is meant to be run on MacOS, Windows, or Linux.")
            url = ("https://s3.amazonaws.com/boxes.10gen.com/build/curator/"
                   "curator-dist-%s-%s.tar.gz") % (os_platform, git_hash)
            response = requests.get(url, stream=True)
            with tarfile.open(mode="r|gz", fileobj=response.raw) as tf:
                tf.extractall(path="./build/")

        jasper_port = config.BASE_PORT - 1
        jasper_conn_str = "localhost:%d" % jasper_port
        jasper_process.Process.connection_str = jasper_conn_str
        jasper_command = [curator_path, "jasper", "grpc", "--port", str(jasper_port)]
        self._jasper_server = process.Process(self._resmoke_logger, jasper_command)
        self._jasper_server.start()

        channel = grpc.insecure_channel(jasper_conn_str)
        grpc.channel_ready_future(channel).result()

    def _exit_jasper(self):
        if self._jasper_server:
            self._jasper_server.stop()

    def exit(self, exit_code):
        """Exit with the provided exit code."""
        self._exit_code = exit_code
        self._resmoke_logger.info("Exiting with code: %d", exit_code)
        sys.exit(exit_code)


def main():
    """Execute Main function for resmoke."""
    resmoke = Resmoke()
    resmoke.configure_from_command_line()
    resmoke.run()


if __name__ == "__main__":
    main()
