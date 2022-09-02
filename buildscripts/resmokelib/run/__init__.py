"""Command line utility for executing MongoDB tests of all kinds."""

import argparse
import collections
import os
import os.path
import random
import shlex
import sys
import textwrap
import time
import shutil

import curatorbin
import pkg_resources
import psutil

from buildscripts.resmokelib import parser as main_parser
from buildscripts.resmokelib import config
from buildscripts.resmokelib import configure_resmoke
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import logging
from buildscripts.resmokelib import reportfile
from buildscripts.resmokelib import sighandler
from buildscripts.resmokelib import suitesconfig
from buildscripts.resmokelib import testing
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.run import generate_multiversion_exclude_tags
from buildscripts.resmokelib.run import runtime_recorder
from buildscripts.resmokelib.run import list_tags
from buildscripts.resmokelib.run.runtime_recorder import compare_start_time
from buildscripts.resmokelib.suitesconfig import get_suite_files

_INTERNAL_OPTIONS_TITLE = "Internal Options"
_MONGODB_SERVER_OPTIONS_TITLE = "MongoDB Server Options"
_BENCHMARK_ARGUMENT_TITLE = "Benchmark/Benchrun test options"
_EVERGREEN_ARGUMENT_TITLE = "Evergreen options"
_CEDAR_ARGUMENT_TITLE = "Cedar options"


class TestRunner(Subcommand):
    """The main class to run tests with resmoke."""

    def __init__(self, command, start_time=time.time()):
        """Initialize the Resmoke instance."""
        self.__start_time = start_time
        self.__command = command
        self._exec_logger = None
        self._resmoke_logger = None
        self._archive = None
        self._interrupted = False
        self._exit_code = 0
        runtime_recorder.setup_start_time(start_time)

    def _setup_logging(self):
        logging.loggers.configure_loggers()
        logging.flush.start_thread()
        self._exec_logger = logging.loggers.ROOT_EXECUTOR_LOGGER
        self._resmoke_logger = logging.loggers.new_resmoke_logger()

    def _exit_logging(self):
        if self._interrupted:
            # We want to exit as quickly as possible when interrupted by a user and therefore don't
            # bother waiting for all log output to be flushed to logkeeper.
            return

        if logging.buildlogger.is_log_output_incomplete():
            # If we already failed to write log output to logkeeper, then we don't bother waiting
            # for any remaining log output to be flushed as it'll likely fail too. Exiting without
            # joining the flush thread here also means that resmoke.py won't hang due a logger from
            # a fixture or a background hook not being closed.
            self._exit_on_incomplete_logging()

        flush_success = logging.flush.stop_thread()
        if not flush_success:
            self._resmoke_logger.error(
                'Failed to flush all logs within a reasonable amount of time, '
                'treating logs as incomplete')

        if not flush_success or logging.buildlogger.is_log_output_incomplete():
            self._exit_on_incomplete_logging()

    def _exit_on_incomplete_logging(self):
        if self._exit_code == 0:
            # We don't anticipate users to look at passing Evergreen tasks very often that even if
            # the log output is incomplete, we'd still rather not show anything in the Evergreen UI
            # or cause a JIRA ticket to be created.
            self._resmoke_logger.info(
                "We failed to flush all log output to logkeeper but all tests passed, so"
                " ignoring.")
        else:
            exit_code = errors.LoggerRuntimeConfigError.EXIT_CODE
            self._resmoke_logger.info(
                "Exiting with code %d rather than requested code %d because we failed to flush all"
                " log output to logkeeper.", exit_code, self._exit_code)
            self._exit_code = exit_code

        # Force exit the process without cleaning up or calling the finally block
        # to avoid threads making system calls from blocking process termination.
        # This must be the last line of code that is run.
        # pylint: disable=protected-access
        os._exit(self._exit_code)

    def execute(self):
        """Execute the 'run' subcommand."""

        self._setup_logging()

        try:
            if self.__command == "list-suites":
                self.list_suites()
            elif self.__command == "find-suites":
                self.find_suites()
            elif self.__command == "list-tags":
                self.list_tags()
            elif self.__command == "generate-multiversion-exclude-tags":
                self.generate_multiversion_exclude_tags()
            elif config.DRY_RUN == "tests":
                self.dry_run()
            else:
                self.run_tests()
        finally:
            # self._exit_logging() may never return when the log output is incomplete.
            # Our workaround is to call os._exit().
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

    def list_tags(self):
        """
        List the tags and its documentation available in the suites.

        Note: this currently ignores composed/matrix suites as it's not obvious the suite
        a particular tag applies to.
        """
        tag_docs = {}
        out_tag_names = []
        for suite_name, suite_file in get_suite_files().items():
            # Matrix suites are ignored.
            tags_blocks = list_tags.get_tags_blocks(suite_file)

            for tags_block in tags_blocks:
                splitted_tags_block = list_tags.split_into_tags(tags_block)

                for single_tag_block in splitted_tags_block:
                    tag_name, doc = list_tags.get_tag_doc(single_tag_block)

                    if tag_name and (tag_name not in tag_docs
                                     or len(doc) > len(tag_docs[tag_name])):
                        tag_docs[tag_name] = doc

                    if suite_name in config.SUITE_FILES:  # pylint: disable=unsupported-membership-test
                        out_tag_names.append(tag_name)

        if config.SUITE_FILES == [config.DEFAULTS["suite_files"]]:
            out_tag_docs = tag_docs
        else:
            out_tag_docs = {tag: doc for tag, doc in tag_docs.items() if tag in out_tag_names}

        self._resmoke_logger.info("Found tags in suites:%s", list_tags.make_output(out_tag_docs))

    def generate_multiversion_exclude_tags(self):
        """Generate multiversion exclude tags file."""
        generate_multiversion_exclude_tags.generate_exclude_yaml(
            config.MULTIVERSION_BIN_VERSION, config.EXCLUDE_TAGS_FILE_PATH, self._resmoke_logger)

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
        self._resmoke_logger.info("verbatim resmoke.py invocation: %s",
                                  " ".join([shlex.quote(arg) for arg in sys.argv]))
        self._check_for_mongo_processes()

        if config.EVERGREEN_TASK_DOC:
            self._resmoke_logger.info("Evergreen task documentation:\n%s",
                                      config.EVERGREEN_TASK_DOC)
        elif config.EVERGREEN_TASK_NAME:
            self._resmoke_logger.info("Evergreen task documentation is absent for this task.")
            task_name = utils.get_task_name_without_suffix(config.EVERGREEN_TASK_NAME,
                                                           config.EVERGREEN_VARIANT_NAME)
            self._resmoke_logger.info(
                "If you are familiar with the functionality of %s task, "
                "please consider adding documentation for it in %s", task_name,
                os.path.join(config.CONFIG_DIR, "evg_task_doc", "evg_task_doc.yml"))

        self._log_local_resmoke_invocation()
        from buildscripts.resmokelib import multiversionconstants
        multiversionconstants.log_constants(self._resmoke_logger)

        suites = None
        try:
            suites = self._get_suites()
            self._setup_archival()
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

    def _log_local_resmoke_invocation(self):
        """Log local resmoke invocation example."""
        local_args = to_local_args()
        local_resmoke_invocation = (
            f"{os.path.join('buildscripts', 'resmoke.py')} {' '.join(local_args)}")

        if config.FUZZ_MONGOD_CONFIGS:
            local_args = strip_fuzz_config_params(local_args)
            local_resmoke_invocation = (
                f"{os.path.join('buildscripts', 'resmoke.py')} {' '.join(local_args)}"
                f" --fuzzMongodConfigs --configFuzzSeed={str(config.CONFIG_FUZZ_SEED)}")

            self._resmoke_logger.info("Fuzzed mongodSetParameters:\n%s",
                                      config.MONGOD_SET_PARAMETERS)
            self._resmoke_logger.info("Fuzzed wiredTigerConnectionString: %s",
                                      config.WT_ENGINE_CONFIG)
        resmoke_env_options = ''
        if os.path.exists('resmoke_env_options.txt'):
            with open('resmoke_env_options.txt') as fin:
                resmoke_env_options = fin.read().strip()

        self._resmoke_logger.info("resmoke.py invocation for local usage: %s %s",
                                  resmoke_env_options, local_resmoke_invocation)

        if config.EVERGREEN_TASK_ID:
            with open("local-resmoke-invocation.txt", "w") as fh:
                fh.write(f"{resmoke_env_options} {local_resmoke_invocation}")

    def _check_for_mongo_processes(self):
        """Check for existing mongo processes as they could interfere with running the tests."""

        if config.AUTO_KILL == 'off' or config.SHELL_CONN_STRING is not None:
            return

        rogue_procs = []
        # Iterate over all running process
        for proc in psutil.process_iter():
            try:
                parent_resmoke_pid = proc.environ().get('RESMOKE_PARENT_PROCESS')
                parent_resmoke_ctime = proc.environ().get('RESMOKE_PARENT_CTIME')
                if not parent_resmoke_pid:
                    continue
                if psutil.pid_exists(int(parent_resmoke_pid)):
                    # Double check `parent_resmoke_pid` is really a rooting resmoke process. Having
                    # the RESMOKE_PARENT_PROCESS environment variable proves it is a process which
                    # was spawned through resmoke. Only a resmoke process has RESMOKE_PARENT_PROCESS
                    # as the value of its own PID.
                    parent_resmoke_proc = psutil.Process(int(parent_resmoke_pid))
                    if parent_resmoke_ctime == str(parent_resmoke_proc.create_time()):
                        continue

                rogue_procs.append(proc)

            except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                pass

        if rogue_procs:
            msg = "detected existing mongo processes. Please clean up these processes as they may affect tests:"

            if config.AUTO_KILL == 'on':
                msg += textwrap.dedent("""\

                    Congratulations, you have selected auto kill mode:
                    HASTA LA VISTA MONGO""" + r"""
                                          ______
                                         <((((((\\\
                                         /      . }\
                                         ;--..--._|}
                      (\                 '--/\--'  )
                       \\                | '-'  :'|
                        \\               . -==- .-|
                         \\               \.__.'   \--._
                         [\\          __.--|       //  _/'--.
                         \ \\       .'-._ ('-----'/ __/      \\
                          \ \\     /   __>|      | '--.       |
                           \ \\   |   \   |     /    /       /
                            \ '\ /     \  |     |  _/       /
                             \  \       \ |     | /        /
                              \  \      \        /
                    """)
                print(f"WARNING: {msg}")
            else:
                self._resmoke_logger.error("ERROR: %s", msg)

            for proc in rogue_procs:
                if config.AUTO_KILL == 'on':
                    proc_msg = f"    Target acquired: pid: {str(proc.pid).ljust(5)} name: {proc.exe()}"
                    try:
                        proc.kill()
                    except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess) as exc:
                        proc_msg += f" - target escaped: {type(exc).__name__ }"
                    else:
                        proc_msg += " - target destroyed\n"
                    print(proc_msg)

                else:
                    self._resmoke_logger.error("    pid: %s name: %s",
                                               str(proc.pid).ljust(5), proc.exe())

            if config.AUTO_KILL == 'on':
                print("I'll be back...\n")
            else:
                raise errors.ResmokeError(
                    textwrap.dedent("""\
                Failing because existing mongo processes detected.
                You can use --autoKillResmokeMongo=on to automatically kill the processes,
                or --autoKillResmokeMongo=off to ignore them.
                """))

    def _log_resmoke_summary(self, suites):
        """Log a summary of the resmoke run."""
        time_taken = time.time() - self.__start_time
        if len(config.SUITE_FILES) > 1:
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
        except OSError as err:
            self._exec_logger.error("Encountered an OSError: %s", err)
            suite.return_code = 74  # Exit code for OSError on POSIX systems.
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

    # pylint: disable=inconsistent-return-statements
    def _get_suites(self):
        """Return the list of suites for this resmoke invocation."""
        try:
            return suitesconfig.get_suites(config.SUITE_FILES, config.TEST_FILES)
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
            utils.dump_yaml({"logging": config.LOGGING_CONFIG})
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

    def exit(self, exit_code):
        """Exit with the provided exit code."""
        self._exit_code = exit_code
        self._resmoke_logger.info("Exiting with code: %d", exit_code)
        sys.exit(exit_code)


_TagInfo = collections.namedtuple("_TagInfo", ["tag_name", "evergreen_aware", "suite_options"])


class TestRunnerEvg(TestRunner):
    """Execute Main class.

    A class for executing potentially multiple resmoke.py test suites in a way that handles
    additional options for running unreliable tests in Evergreen.
    """

    UNRELIABLE_TAG = _TagInfo(
        tag_name="unreliable",
        evergreen_aware=True,
        suite_options=config.SuiteOptions.ALL_INHERITED._replace(  # type: ignore
            report_failure_status="silentfail"))

    RESOURCE_INTENSIVE_TAG = _TagInfo(
        tag_name="resource_intensive",
        evergreen_aware=False,
        suite_options=config.SuiteOptions.ALL_INHERITED._replace(  # type: ignore
            num_jobs=1))

    RETRY_ON_FAILURE_TAG = _TagInfo(
        tag_name="retry_on_failure",
        evergreen_aware=True,
        suite_options=config.SuiteOptions.ALL_INHERITED._replace(  # type: ignore
            fail_fast=False, num_repeat_suites=2, num_repeat_tests=1,
            report_failure_status="silentfail"))

    @staticmethod
    def _make_evergreen_aware_tags(tag_name):
        """Return a list of resmoke.py tags.

        This list is for task, variant, and distro combinations in Evergreen.
        """

        tags_format = ["{tag_name}"]

        if config.EVERGREEN_TASK_NAME is not None:
            tags_format.append("{tag_name}|{task_name}")

            if config.EVERGREEN_VARIANT_NAME is not None:
                tags_format.append("{tag_name}|{task_name}|{variant_name}")

                if config.EVERGREEN_DISTRO_ID is not None:
                    tags_format.append("{tag_name}|{task_name}|{variant_name}|{distro_id}")

        return [
            tag.format(tag_name=tag_name, task_name=config.EVERGREEN_TASK_NAME,
                       variant_name=config.EVERGREEN_VARIANT_NAME,
                       distro_id=config.EVERGREEN_DISTRO_ID) for tag in tags_format
        ]

    @classmethod
    def _make_tag_combinations(cls):
        """Return a list of (tag, enabled) pairs.

        These pairs represent all possible combinations of all possible pairings
        of whether the tags are enabled or disabled together.
        """

        combinations = []

        if config.EVERGREEN_PATCH_BUILD:
            combinations.append(("unreliable and resource intensive",
                                 ((cls.UNRELIABLE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG, True))))
            combinations.append(("unreliable and not resource intensive",
                                 ((cls.UNRELIABLE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG, False))))
            combinations.append(("reliable and resource intensive",
                                 ((cls.UNRELIABLE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG, True))))
            combinations.append(("reliable and not resource intensive",
                                 ((cls.UNRELIABLE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG,
                                                                False))))
        else:
            combinations.append(("retry on failure and resource intensive",
                                 ((cls.RETRY_ON_FAILURE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG,
                                                                     True))))
            combinations.append(("retry on failure and not resource intensive",
                                 ((cls.RETRY_ON_FAILURE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG,
                                                                     False))))
            combinations.append(("run once and resource intensive",
                                 ((cls.RETRY_ON_FAILURE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG,
                                                                      True))))
            combinations.append(("run once and not resource intensive",
                                 ((cls.RETRY_ON_FAILURE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG,
                                                                      False))))

        return combinations

    def _get_suites(self):
        """Return a list of resmokelib.testing.suite.Suite instances to execute.

        For every resmokelib.testing.suite.Suite instance returned by resmoke.Main._get_suites(),
        multiple copies of that test suite are run using different resmokelib.config.SuiteOptions()
        depending on whether each tag in the combination is enabled or not.
        """

        suites = []

        for suite in TestRunner._get_suites(self):
            if suite.test_kind != "js_test":
                # Tags are only support for JavaScript tests, so we leave the test suite alone when
                # running any other kind of test.
                suites.append(suite)
                continue

            for (tag_desc, tag_combo) in self._make_tag_combinations():
                suite_options_list = []

                for (tag_info, enabled) in tag_combo:
                    if tag_info.evergreen_aware:
                        tags = self._make_evergreen_aware_tags(tag_info.tag_name)
                        include_tags = {"$anyOf": tags}
                    else:
                        include_tags = tag_info.tag_name

                    if enabled:
                        suite_options = tag_info.suite_options._replace(include_tags=include_tags)
                    else:
                        suite_options = config.SuiteOptions.ALL_INHERITED._replace(
                            include_tags={"$not": include_tags})

                    suite_options_list.append(suite_options)

                suite_options = config.SuiteOptions.combine(*suite_options_list)
                suite_options = suite_options._replace(description=tag_desc)
                suites.append(suite.with_options(suite_options))

        return suites


class RunPlugin(PluginInterface):
    """Interface to parsing."""

    def add_subcommand(self, subparsers):
        """
        Add subcommand parser.

        :param subparsers: argparse subparsers
        """
        RunPlugin._add_run(subparsers)
        RunPlugin._add_list_suites(subparsers)
        RunPlugin._add_find_suites(subparsers)
        RunPlugin._add_list_tags(subparsers)
        RunPlugin._add_generate_multiversion_exclude_tags(subparsers)

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return Run Subcommand if command is one we recognize.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """
        if subcommand in ('find-suites', 'list-suites', 'list-tags', 'run',
                          'generate-multiversion-exclude-tags'):
            configure_resmoke.validate_and_update_config(parser, parsed_args)
            if config.EVERGREEN_TASK_ID is not None:
                return TestRunnerEvg(subcommand, **kwargs)
            else:
                return TestRunner(subcommand, **kwargs)
        return None

    @classmethod
    def _add_run(cls, subparsers):
        """Create and add the parser for the Run subcommand."""
        parser = subparsers.add_parser("run", help="Runs the specified tests.")

        parser.set_defaults(dry_run="off", shuffle="auto", stagger_jobs="off",
                            majority_read_concern="on")

        parser.add_argument("test_files", metavar="TEST_FILES", nargs="*",
                            help="Explicit test files to run")

        parser.add_argument(
            "--suites", dest="suite_files", metavar="SUITE1,SUITE2",
            help=("Comma separated list of YAML files that each specify the configuration"
                  " of a suite. If the file is located in the resmokeconfig/suites/"
                  " directory, then the basename without the .yml extension can be"
                  " specified, e.g. 'core'. If a list of files is passed in as"
                  " positional arguments, they will be run using the suites'"
                  " configurations."))

        parser.add_argument(
            "--autoKillResmokeMongo", dest="auto_kill", choices=['on', 'error',
                                                                 'off'], default='on',
            help=("When resmoke starts up, existing mongo processes created from resmoke "
                  " could cause issues when running tests. This option causes resmoke to kill"
                  " the existing processes and continue running the test, or if 'error' option"
                  " is used, prints the offending processes and fails the test."))

        parser.add_argument("--installDir", dest="install_dir", metavar="INSTALL_DIR",
                            help="Directory to search for MongoDB binaries")

        parser.add_argument(
            "--alwaysUseLogFiles", dest="always_use_log_files", action="store_true",
            help=("Logs server output to a file located in the db path and prevents the"
                  " cleaning of dbpaths after testing. Note that conflicting options"
                  " passed in from test files may cause an error."))

        parser.add_argument(
            "--basePort", dest="base_port", metavar="PORT",
            help=("The starting port number to use for mongod and mongos processes"
                  " spawned by resmoke.py or the tests themselves. Each fixture and Job"
                  " allocates a contiguous range of ports."))

        parser.add_argument("--continueOnFailure", action="store_true", dest="continue_on_failure",
                            help="Executes all tests in all suites, even if some of them fail.")

        parser.add_argument("--dbtest", dest="dbtest_executable", metavar="PATH",
                            help="The path to the dbtest executable for resmoke to use.")

        parser.add_argument(
            "--excludeWithAnyTags", action="append", dest="exclude_with_any_tags",
            metavar="TAG1,TAG2",
            help=("Comma separated list of tags. Any jstest that contains any of the"
                  " specified tags will be excluded from any suites that are run."
                  " The tag '{}' is implicitly part of this list.".format(config.EXCLUDED_TAG)))

        parser.add_argument("--genny", dest="genny_executable", metavar="PATH",
                            help="The path to the genny executable for resmoke to use.")

        parser.add_argument(
            "--includeWithAnyTags", action="append", dest="include_with_any_tags",
            metavar="TAG1,TAG2",
            help=("Comma separated list of tags. For the jstest portion of the suite(s),"
                  " only tests which have at least one of the specified tags will be"
                  " run."))

        parser.add_argument(
            "--includeWithAllTags", action="append", dest="include_with_all_tags",
            metavar="TAG1,TAG2",
            help=("Comma separated list of tags. For the jstest portion of the suite(s),"
                  "tests that have all of the specified tags will be run."))

        parser.add_argument("-n", action="store_const", const="tests", dest="dry_run",
                            help="Outputs the tests that would be run.")

        parser.add_argument(
            "--recordWith", dest="undo_recorder_path", metavar="PATH",
            help="Record execution of mongo, mongod and mongos processes;"
            "specify the path to UndoDB's 'live-record' binary")

        # TODO: add support for --dryRun=commands
        parser.add_argument(
            "--dryRun", action="store", dest="dry_run", choices=("off", "tests"), metavar="MODE",
            help=("Instead of running the tests, outputs the tests that would be run"
                  " (if MODE=tests). Defaults to MODE=%(default)s."))

        parser.add_argument(
            "-j", "--jobs", type=int, dest="jobs", metavar="JOBS",
            help=("The number of Job instances to use. Each instance will receive its"
                  " own MongoDB deployment to dispatch tests to."))

        parser.set_defaults(logger_file="console")

        parser.add_argument(
            "--mongocryptdSetParameters", dest="mongocryptd_set_parameters", action="append",
            metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
            help=("Passes one or more --setParameter options to all mongocryptd processes"
                  " started by resmoke.py. The argument is specified as bracketed YAML -"
                  " i.e. JSON with support for single quoted and unquoted keys."))

        parser.add_argument("--numClientsPerFixture", type=int, dest="num_clients_per_fixture",
                            help="Number of clients running tests per fixture.")

        parser.add_argument(
            "--shellConnString", dest="shell_conn_string", metavar="CONN_STRING",
            help="Overrides the default fixture and connects with a mongodb:// connection"
            " string to an existing MongoDB cluster instead. This is useful for"
            " connecting to a MongoDB deployment started outside of resmoke.py including"
            " one running in a debugger.")

        parser.add_argument(
            "--shellPort", dest="shell_port", metavar="PORT",
            help="Convenience form of --shellConnString for connecting to an"
            " existing MongoDB cluster with the URL mongodb://localhost:[PORT]."
            " This is useful for connecting to a server running in a debugger.")

        parser.add_argument("--repeat", "--repeatSuites", type=int, dest="repeat_suites",
                            metavar="N",
                            help="Repeats the given suite(s) N times, or until one fails.")

        parser.add_argument(
            "--repeatTests", type=int, dest="repeat_tests", metavar="N",
            help="Repeats the tests inside each suite N times. This applies to tests"
            " defined in the suite configuration as well as tests defined on the command"
            " line.")

        parser.add_argument(
            "--repeatTestsMax", type=int, dest="repeat_tests_max", metavar="N",
            help="Repeats the tests inside each suite no more than N time when"
            " --repeatTestsSecs is specified. This applies to tests defined in the suite"
            " configuration as well as tests defined on the command line.")

        parser.add_argument(
            "--repeatTestsMin", type=int, dest="repeat_tests_min", metavar="N",
            help="Repeats the tests inside each suite at least N times when"
            " --repeatTestsSecs is specified. This applies to tests defined in the suite"
            " configuration as well as tests defined on the command line.")

        parser.add_argument(
            "--repeatTestsSecs", type=float, dest="repeat_tests_secs", metavar="SECONDS",
            help="Repeats the tests inside each suite this amount of time. Note that"
            " this option is mutually exclusive with --repeatTests. This applies to"
            " tests defined in the suite configuration as well as tests defined on the"
            " command line.")

        parser.add_argument(
            "--seed", type=int, dest="seed", metavar="SEED",
            help=("Seed for the random number generator. Useful in combination with the"
                  " --shuffle option for producing a consistent test execution order."))

        parser.add_argument("--mongo", dest="mongo_executable", metavar="PATH",
                            help="The path to the mongo shell executable for resmoke.py to use.")

        parser.add_argument(
            "--shuffle", action="store_const", const="on", dest="shuffle",
            help=("Randomizes the order in which tests are executed. This is equivalent"
                  " to specifying --shuffleMode=on."))

        parser.add_argument(
            "--shuffleMode", action="store", dest="shuffle", choices=("on", "off", "auto"),
            metavar="ON|OFF|AUTO",
            help=("Controls whether to randomize the order in which tests are executed."
                  " Defaults to auto when not supplied. auto enables randomization in"
                  " all cases except when the number of jobs requested is 1."))

        parser.add_argument(
            "--executor", dest="executor_file",
            help="OBSOLETE: Superceded by --suites; specify --suites=SUITE path/to/test"
            " to run a particular test under a particular suite configuration.")

        parser.add_argument(
            "--linearChain", action="store", dest="linear_chain", choices=("on", "off"),
            metavar="ON|OFF", help="Enable or disable linear chaining for tests using "
            "ReplicaSetFixture.")

        parser.add_argument(
            "--backupOnRestartDir", action="store", type=str, dest="backup_on_restart_dir",
            metavar="DIRECTORY", help=
            "Every time a mongod restarts on existing data files, the data files will be backed up underneath the input directory."
        )

        parser.add_argument(
            "--replayFile", action="store", type=str, dest="replay_file", metavar="FILE", help=
            "Run the tests listed in the input file. This is an alternative to passing test files as positional arguments on the command line. Each line in the file must be a path to a test file relative to the current working directory. A short-hand for `resmoke run --replay_file foo` is `resmoke run @foo`."
        )

        parser.add_argument(
            "--mrlog", action="store_const", const="mrlog", dest="mrlog", help=
            "Pipe output through the `mrlog` binary for converting logv2 logs to human readable logs."
        )

        parser.add_argument(
            "--userFriendlyOutput", action="store", type=str, dest="user_friendly_output",
            metavar="FILE", help=
            "Have resmoke redirect all output to FILE. Additionally, stdout will contain lines that typically indicate that the test is making progress, or an error has happened. If `mrlog` is in the path it will be used. `tee` and `egrep` must be in the path."
        )

        parser.add_argument(
            "--runAllFeatureFlagTests", dest="run_all_feature_flag_tests", action="store_true",
            help=
            "Run MongoDB servers with all feature flags enabled and only run tests tags with these feature flags"
        )

        parser.add_argument(
            "--runAllFeatureFlagsNoTests", dest="run_all_feature_flags_no_tests",
            action="store_true", help=
            "Run MongoDB servers with all feature flags enabled but don't run any tests tagged with these feature flags; used for multiversion suites"
        )

        parser.add_argument("--additionalFeatureFlags", dest="additional_feature_flags",
                            action="append", metavar="featureFlag1, featureFlag2, ...",
                            help="Additional feature flags")

        parser.add_argument("--maxTestQueueSize", type=int, dest="max_test_queue_size",
                            help=argparse.SUPPRESS)

        mongodb_server_options = parser.add_argument_group(
            title=_MONGODB_SERVER_OPTIONS_TITLE,
            description=("Options related to starting a MongoDB cluster that are forwarded from"
                         " resmoke.py to the fixture."))

        mongodb_server_options.add_argument(
            "--mongod", dest="mongod_executable", metavar="PATH",
            help="The path to the mongod executable for resmoke.py to use.")

        mongodb_server_options.add_argument(
            "--mongos", dest="mongos_executable", metavar="PATH",
            help="The path to the mongos executable for resmoke.py to use.")

        mongodb_server_options.add_argument(
            "--mongodSetParameters", dest="mongod_set_parameters", action="append",
            metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
            help=("Passes one or more --setParameter options to all mongod processes"
                  " started by resmoke.py. The argument is specified as bracketed YAML -"
                  " i.e. JSON with support for single quoted and unquoted keys."))

        mongodb_server_options.add_argument(
            "--mongosSetParameters", dest="mongos_set_parameters", action="append",
            metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
            help=("Passes one or more --setParameter options to all mongos processes"
                  " started by resmoke.py. The argument is specified as bracketed YAML -"
                  " i.e. JSON with support for single quoted and unquoted keys."))

        mongodb_server_options.add_argument(
            "--dbpathPrefix", dest="dbpath_prefix", metavar="PATH",
            help=("The directory which will contain the dbpaths of any mongod's started"
                  " by resmoke.py or the tests themselves."))

        mongodb_server_options.add_argument(
            "--majorityReadConcern", action="store", dest="majority_read_concern", choices=("on",
                                                                                            "off"),
            metavar="ON|OFF", help=("Enable or disable majority read concern support."
                                    " Defaults to %(default)s."))

        mongodb_server_options.add_argument("--flowControl", action="store", dest="flow_control",
                                            choices=("on", "off"), metavar="ON|OFF",
                                            help=("Enable or disable flow control."))

        mongodb_server_options.add_argument("--flowControlTicketOverride", type=int, action="store",
                                            dest="flow_control_tickets", metavar="TICKET_OVERRIDE",
                                            help=("Number of tickets available for flow control."))

        mongodb_server_options.add_argument("--storageEngine", dest="storage_engine",
                                            metavar="ENGINE",
                                            help="The storage engine used by dbtests and jstests.")

        mongodb_server_options.add_argument(
            "--storageEngineCacheSizeGB", dest="storage_engine_cache_size_gb", metavar="CONFIG",
            help="Sets the storage engine cache size configuration"
            " setting for all mongod's.")

        mongodb_server_options.add_argument(
            "--numReplSetNodes", type=int, dest="num_replset_nodes", metavar="N",
            help="The number of nodes to initialize per ReplicaSetFixture. This is also "
            "used to indicate the number of replica set members per shard in a "
            "ShardedClusterFixture.")

        mongodb_server_options.add_argument(
            "--numShards", type=int, dest="num_shards", metavar="N",
            help="The number of shards to use in a ShardedClusterFixture.")

        mongodb_server_options.add_argument(
            "--wiredTigerCollectionConfigString", dest="wt_coll_config", metavar="CONFIG",
            help="Sets the WiredTiger collection configuration setting for all mongod's.")

        mongodb_server_options.add_argument(
            "--wiredTigerEngineConfigString", dest="wt_engine_config", metavar="CONFIG",
            help="Sets the WiredTiger engine configuration setting for all mongod's.")

        mongodb_server_options.add_argument(
            "--wiredTigerIndexConfigString", dest="wt_index_config", metavar="CONFIG",
            help="Sets the WiredTiger index configuration setting for all mongod's.")

        mongodb_server_options.add_argument("--transportLayer", dest="transport_layer",
                                            metavar="TRANSPORT",
                                            help="The transport layer used by jstests")

        mongodb_server_options.add_argument(
            "--fuzzMongodConfigs", dest="fuzz_mongod_configs", action="store_true",
            help="Will randomly choose storage configs that were not specified.")

        mongodb_server_options.add_argument("--configFuzzSeed", dest="config_fuzz_seed",
                                            metavar="PATH",
                                            help="Sets the seed used by storage config fuzzer")

        internal_options = parser.add_argument_group(
            title=_INTERNAL_OPTIONS_TITLE,
            description=("Internal options for advanced users and resmoke developers."
                         " These are not meant to be invoked when running resmoke locally."))

        internal_options.add_argument(
            "--log", dest="logger_file", metavar="LOGGER",
            help=("A YAML file that specifies the logging configuration. If the file is"
                  " located in the resmokeconfig/suites/ directory, then the basename"
                  " without the .yml extension can be specified, e.g. 'console'."))

        # Used for testing resmoke.
        #
        # `is_inner_level`:
        #     Marks the resmoke process as a child of a parent resmoke process, meaning that"
        #     it was started by a shell process which itself was started by a top-level"
        #     resmoke process. This is used to ensure the hang-analyzer is called properly."
        #
        # `test_archival`:
        #     Allows unit testing of resmoke's archival feature where we write out the names
        #     of the files to be archived, instead of doing the actual archival, which can
        #     be time and resource intensive.
        #
        # `test_analysis`:
        #     When specified, the hang-analyzer writes out the pids it will analyze without
        #     actually running analysis, which can be time and resource intensive.
        internal_options.add_argument("--internalParam", action="append", dest="internal_params",
                                      help=argparse.SUPPRESS)

        internal_options.add_argument("--perfReportFile", dest="perf_report_file",
                                      metavar="PERF_REPORT",
                                      help="Writes a JSON file with performance test results.")

        internal_options.add_argument("--cedarReportFile", dest="cedar_report_file",
                                      metavar="CEDAR_REPORT",
                                      help="Writes a JSON file with performance test results.")

        internal_options.add_argument(
            "--reportFailureStatus", action="store", dest="report_failure_status",
            choices=("fail", "silentfail"), metavar="STATUS",
            help="Controls if the test failure status should be reported as failed"
            " or be silently ignored (STATUS=silentfail). Dynamic test failures will"
            " never be silently ignored. Defaults to STATUS=%(default)s.")

        internal_options.add_argument(
            "--reportFile", dest="report_file", metavar="REPORT",
            help="Writes a JSON file with test status and timing information.")

        internal_options.add_argument(
            "--staggerJobs", action="store", dest="stagger_jobs", choices=("on", "off"),
            metavar="ON|OFF", help=("Enables or disables the stagger of launching resmoke jobs."
                                    " Defaults to %(default)s."))

        internal_options.add_argument(
            "--exportMongodConfig", dest="export_mongod_config", choices=("off", "regular",
                                                                          "detailed"),
            help=("Exports a yaml containing the history of each mongod config option to"
                  " {nodeName}_config.yml."
                  " Defaults to 'off'. A 'detailed' export will include locations of accesses."))

        evergreen_options = parser.add_argument_group(
            title=_EVERGREEN_ARGUMENT_TITLE, description=(
                "Options used to propagate information about the Evergreen task running this"
                " script."))

        evergreen_options.add_argument("--evergreenURL", dest="evergreen_url",
                                       metavar="EVERGREEN_URL",
                                       help=("The URL of the Evergreen service."))

        evergreen_options.add_argument(
            "--archiveLimitMb", type=int, dest="archive_limit_mb", metavar="ARCHIVE_LIMIT_MB",
            help=("Sets the limit (in MB) for archived files to S3. A value of 0"
                  " indicates there is no limit."))

        evergreen_options.add_argument(
            "--archiveLimitTests", type=int, dest="archive_limit_tests",
            metavar="ARCHIVE_LIMIT_TESTS",
            help=("Sets the maximum number of tests to archive to S3. A value"
                  " of 0 indicates there is no limit."))

        evergreen_options.add_argument("--buildId", dest="build_id", metavar="BUILD_ID",
                                       help="Sets the build ID of the task.")

        evergreen_options.add_argument("--buildloggerUrl", action="store", dest="buildlogger_url",
                                       metavar="URL",
                                       help="The root url of the buildlogger server.")

        evergreen_options.add_argument(
            "--distroId", dest="distro_id", metavar="DISTRO_ID",
            help=("Sets the identifier for the Evergreen distro running the"
                  " tests."))

        evergreen_options.add_argument(
            "--executionNumber", type=int, dest="execution_number", metavar="EXECUTION_NUMBER",
            help=("Sets the number for the Evergreen execution running the"
                  " tests."))

        evergreen_options.add_argument(
            "--gitRevision", dest="git_revision", metavar="GIT_REVISION",
            help=("Sets the git revision for the Evergreen task running the"
                  " tests."))

        # We intentionally avoid adding a new command line option that starts with --suite so it doesn't
        # become ambiguous with the --suites option and break how engineers run resmoke.py locally.
        evergreen_options.add_argument(
            "--originSuite", dest="origin_suite", metavar="SUITE",
            help=("Indicates the name of the test suite prior to the"
                  " evergreen_generate_resmoke_tasks.py script splitting it"
                  " up."))

        evergreen_options.add_argument(
            "--patchBuild", action="store_true", dest="patch_build",
            help=("Indicates that the Evergreen task running the tests is a"
                  " patch build."))

        evergreen_options.add_argument(
            "--projectName", dest="project_name", metavar="PROJECT_NAME",
            help=("Sets the name of the Evergreen project running the tests."))

        evergreen_options.add_argument("--revisionOrderId", dest="revision_order_id",
                                       metavar="REVISION_ORDER_ID",
                                       help="Sets the chronological order number of this commit.")

        evergreen_options.add_argument("--tagFile", action="append", dest="tag_files",
                                       metavar="TAG_FILES",
                                       help="One or more YAML files that associate tests and tags.")

        evergreen_options.add_argument(
            "--taskName", dest="task_name", metavar="TASK_NAME",
            help="Sets the name of the Evergreen task running the tests.")

        evergreen_options.add_argument("--taskId", dest="task_id", metavar="TASK_ID",
                                       help="Sets the Id of the Evergreen task running the tests.")

        evergreen_options.add_argument(
            "--variantName", dest="variant_name", metavar="VARIANT_NAME",
            help=("Sets the name of the Evergreen build variant running the"
                  " tests."))

        evergreen_options.add_argument("--versionId", dest="version_id", metavar="VERSION_ID",
                                       help="Sets the version ID of the task.")

        benchmark_options = parser.add_argument_group(
            title=_BENCHMARK_ARGUMENT_TITLE,
            description="Options for running Benchmark/Benchrun tests")

        benchmark_options.add_argument("--benchmarkFilter", type=str, dest="benchmark_filter",
                                       metavar="BENCHMARK_FILTER",
                                       help="Regex to filter Google benchmark tests to run.")

        benchmark_options.add_argument(
            "--benchmarkListTests",
            dest="benchmark_list_tests",
            action="store_true",
            # metavar="BENCHMARK_LIST_TESTS",
            help=("Lists all Google benchmark test configurations in each"
                  " test file."))

        benchmark_min_time_help = (
            "Minimum time to run each benchmark/benchrun test for. Use this option instead of "
            "--benchmarkRepetitions to make a test run for a longer or shorter duration.")
        benchmark_options.add_argument("--benchmarkMinTimeSecs", type=int,
                                       dest="benchmark_min_time_secs", metavar="BENCHMARK_MIN_TIME",
                                       help=benchmark_min_time_help)

        benchmark_repetitions_help = (
            "Set --benchmarkRepetitions=1 if you'd like to run the benchmark/benchrun tests only once."
            " By default, each test is run multiple times to provide statistics on the variance"
            " between runs; use --benchmarkMinTimeSecs if you'd like to run a test for a longer or"
            " shorter duration.")
        benchmark_options.add_argument(
            "--benchmarkRepetitions", type=int, dest="benchmark_repetitions",
            metavar="BENCHMARK_REPETITIONS", help=benchmark_repetitions_help)

    @classmethod
    def _add_list_suites(cls, subparsers):
        """Create and add the parser for the list-suites subcommand."""
        parser = subparsers.add_parser("list-suites",
                                       help="Lists the names of the suites available to execute.")

        parser.add_argument(
            "--log", dest="logger_file", metavar="LOGGER",
            help=("A YAML file that specifies the logging configuration. If the file is"
                  " located in the resmokeconfig/suites/ directory, then the basename"
                  " without the .yml extension can be specified, e.g. 'console'."))
        parser.set_defaults(logger_file="console")

    @classmethod
    def _add_find_suites(cls, subparsers):
        """Create and add the parser for the find-suites subcommand."""
        parser = subparsers.add_parser(
            "find-suites",
            help="Lists the names of the suites that will execute the specified tests.")

        # find-suites shares a lot of code with 'run' (for now), and this option needs be specified,
        # though it is not used.
        parser.set_defaults(logger_file="console")

        parser.add_argument("test_files", metavar="TEST_FILES", nargs="*",
                            help="Explicit test files to run")

    @classmethod
    def _add_list_tags(cls, subparsers):
        """Create and add the parser for the list-tags subcommand."""
        parser = subparsers.add_parser(
            "list-tags", help="Lists the tags and their documentation available in the suites.")
        parser.set_defaults(logger_file="console")
        parser.add_argument(
            "--suites", dest="suite_files", metavar="SUITE1,SUITE2",
            help=("Comma separated list of suite names to get tags from."
                  " All suites are used if unspecified."))

    @classmethod
    def _add_generate_multiversion_exclude_tags(cls, subparser):
        """Create and add the parser for the generate-multiversion-exclude-tags subcommand."""
        parser = subparser.add_parser(
            "generate-multiversion-exclude-tags",
            help="Create a tag file associating multiversion tests to tags for exclusion."
            " Compares the BACKPORTS_REQUIRED_FILE on the current branch with the same file on the"
            " last-lts and/or last-continuous branch to determine which tests should be denylisted."
        )
        parser.set_defaults(logger_file="console")
        parser.add_argument(
            "--oldBinVersion", type=str, dest="old_bin_version",
            choices=config.MultiversionOptions.all_options(),
            help="Choose the multiversion binary version as last-lts or last-continuous.")
        parser.add_argument("--excludeTagsFilePath", type=str, dest="exclude_tags_file_path",
                            help="Where to output the generated tags.")


def to_local_args(input_args=None):
    """
    Return a command line invocation for resmoke.py suitable for being run outside of Evergreen.

    This function parses the 'args' list of command line arguments, removes any Evergreen-centric
    options, and returns a new list of command line arguments.
    """

    if input_args is None:
        input_args = sys.argv[1:]

    (parser, parsed_args) = main_parser.parse(input_args)

    if parsed_args.command != 'run':
        raise TypeError(
            f"to_local_args can only be called for the 'run' subcommand. Instead was called on '{parsed_args.command}'"
        )

    # If --originSuite was specified, then we replace the value of --suites with it. This is done to
    # avoid needing to have engineers learn about the test suites generated by the
    # evergreen_generate_resmoke_tasks.py script.
    origin_suite = getattr(parsed_args, "origin_suite", None)
    if origin_suite is not None:
        setattr(parsed_args, "suite_files", origin_suite)

    # Replace --runAllFeatureFlagTests with an explicit list of feature flags. The former relies on
    # all_feature_flags.txt which may not exist in the local dev environment.
    run_all_feature_flag_tests = getattr(parsed_args, "run_all_feature_flag_tests", None)
    if run_all_feature_flag_tests is not None:
        setattr(parsed_args, "additional_feature_flags", config.ENABLED_FEATURE_FLAGS)
        del parsed_args.run_all_feature_flag_tests

    del parsed_args.run_all_feature_flags_no_tests

    # The top-level parser has one subparser that contains all subcommand parsers.
    command_subparser = [
        action for action in parser._actions  # pylint: disable=protected-access
        if action.dest == "command"
    ][0]

    run_parser = command_subparser.choices.get("run")

    suites_arg = None
    storage_engine_arg = None
    other_local_args = []
    positional_args = []

    def format_option(option_name, option_value):
        """
        Return <option_name>=<option_value>.

        This function assumes that 'option_name' is always "--" prefix and isn't "-" prefixed.
        """
        return f"{option_name}={option_value}"

    # Trim the argument namespace of any args we don't want to return.
    for group in run_parser._action_groups:  # pylint: disable=protected-access
        arg_dests_visited = set()
        for action in group._group_actions:  # pylint: disable=protected-access
            arg_dest = action.dest
            arg_value = getattr(parsed_args, arg_dest, None)

            # Some arguments, such as --shuffle and --shuffleMode, update the same dest variable.
            # To not print out multiple arguments that will update the same dest, we will skip once
            # one such argument has been visited.
            if arg_dest in arg_dests_visited:
                continue
            else:
                arg_dests_visited.add(arg_dest)

            # If the arg doesn't exist in the parsed namespace, skip.
            # This is mainly for "--help".
            if not hasattr(parsed_args, arg_dest):
                continue
            # Skip any evergreen centric args.
            elif group.title in [
                    _INTERNAL_OPTIONS_TITLE, _EVERGREEN_ARGUMENT_TITLE, _CEDAR_ARGUMENT_TITLE
            ]:
                continue
            elif group.title == 'positional arguments':
                positional_args.extend(arg_value)
            # Keep all remaining args.
            else:
                arg_name = action.option_strings[-1]

                # If an option has the same value as the default, we don't need to specify it.
                if getattr(parsed_args, arg_dest, None) == action.default:
                    continue
                # These are arguments that take no value.
                elif action.nargs == 0:
                    other_local_args.append(arg_name)
                elif isinstance(action, argparse._AppendAction):  # pylint: disable=protected-access
                    args = [format_option(arg_name, elem) for elem in arg_value]
                    other_local_args.extend(args)
                else:
                    arg = format_option(arg_name, arg_value)

                    # We track the value for the --suites and --storageEngine command line options
                    # separately in order to more easily sort them to the front.
                    if arg_dest == "suite_files":
                        suites_arg = arg
                    elif arg_dest == "storage_engine":
                        storage_engine_arg = arg
                    else:
                        other_local_args.append(arg)

    return ["run"] + [arg for arg in (suites_arg, storage_engine_arg) if arg is not None
                      ] + other_local_args + positional_args


def strip_fuzz_config_params(input_args):
    """Delete fuzz related command line args because we have to add the seed manually."""

    ret = []
    for arg in input_args:
        if "--fuzzMongodConfigs" not in arg and "--fuzzConfigSeed" not in arg:
            ret.append(arg)

    return ret
