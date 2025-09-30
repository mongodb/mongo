"""Command line utility for executing MongoDB tests of all kinds."""

import argparse
import collections
import os
import os.path
import platform
import random
import shlex
import subprocess
import sys
import textwrap
import time
from logging import Logger
from typing import List, Optional

import psutil
from opentelemetry import trace
from opentelemetry.trace.status import StatusCode

from buildscripts.ciconfig.evergreen import parse_evergreen_file
from buildscripts.resmokelib import (
    config,
    configure_resmoke,
    errors,
    logging,
    reportfile,
    sighandler,
    suitesconfig,
    testing,
    utils,
)
from buildscripts.resmokelib import parser as main_parser
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.run import (
    generate_multiversion_exclude_tags,
    list_tags,
    runtime_recorder,
)
from buildscripts.resmokelib.run.runtime_recorder import compare_start_time
from buildscripts.resmokelib.suitesconfig import get_suite_files
from buildscripts.resmokelib.testing.docker_cluster_image_builder import build_images
from buildscripts.resmokelib.testing.suite import Suite
from buildscripts.resmokelib.utils.dictionary import get_dict_value

_INTERNAL_OPTIONS_TITLE = "Internal Options"
_MONGODB_SERVER_OPTIONS_TITLE = "MongoDB Server Options"
_BENCHMARK_ARGUMENT_TITLE = "Benchmark/Benchrun test options"
_EVERGREEN_ARGUMENT_TITLE = "Evergreen options"
_CEDAR_ARGUMENT_TITLE = "Cedar options"

TRACER = trace.get_tracer("resmoke")


class TestRunner(Subcommand):
    """The main class to run tests with resmoke."""

    def __init__(self, command: str, start_time=time.time()):
        """Initialize the Resmoke instance."""
        self.__start_time = start_time
        self.__command = command
        self._exec_logger: Optional[Logger] = None
        self._resmoke_logger: Optional[Logger] = None
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
            elif self.__command == "generate-matrix-suites":
                suitesconfig.generate()
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
            config.MULTIVERSION_BIN_VERSION, config.EXCLUDE_TAGS_FILE_PATH, config.EXPANSIONS_FILE,
            self._resmoke_logger)

    @staticmethod
    def _find_suites_by_test(suites: List[Suite]):
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
        # This code path should only execute when resmoke is running from a workload container.
        if config.REQUIRES_WORKLOAD_CONTAINER_SETUP:
            self._setup_workload_container()

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

    def _setup_workload_container(self):
        """Perform any setup needed to run this resmoke suite from within the workload container."""
        # Right now, this is only needed to setup jstestfuzz suites. We do a simple check
        # of the 'roots' and generate jstestfuzz test files if they are expected.
        #
        # If this expands, we should definitely do this more intelligently.
        # ie: run the exact bash scripts from the corresponding evergreen task defintion
        #     so that everything is setup exactly how it would be in Evergreen.

        jstestfuzz_repo_dir = "/mongo/jstestfuzz"
        jstests_dir = "/mongo/jstests"
        jstestfuzz_tests_dir = "/mongo/jstestfuzz/out"

        # Currently, you can only run one suite at a time from within a workload container
        suite = self._get_suites()[0]
        if "jstestfuzz/out/*.js" in suite.get_selector_config().get("roots", []) and not any(
                filename.endswith(".js") for filename in os.listdir(jstestfuzz_tests_dir)):
            subprocess.run([
                "./src/scripts/npm_run.sh",
                "jstestfuzz",
                "--",
                "--jsTestsDir",
                jstests_dir,
            ], cwd=jstestfuzz_repo_dir, stdout=sys.stdout, stderr=sys.stderr, check=True)

    def _run_suite(self, suite: Suite):
        """Run a test suite."""
        self._log_suite_config(suite)
        suite.record_suite_start()
        interrupted = self._execute_suite(suite)
        suite.record_suite_end()
        self._log_suite_summary(suite)
        return interrupted

    def _log_local_resmoke_invocation(self):
        """Log local resmoke invocation example."""

        # Do not log local args if this is not being ran in evergreen
        if not config.EVERGREEN_TASK_ID:
            print("Skipping local invocation because evergreen task id was not provided.")
            return

        evg_conf = parse_evergreen_file(config.EVERGREEN_PROJECT_CONFIG_PATH)

        suite = self._get_suites()[0]
        suite_name = config.ORIGIN_SUITE or suite.get_name()

        # try to find the evergreen task from the resmoke suite name
        task = evg_conf.get_task(suite_name) or evg_conf.get_task(f"{suite_name}_gen")

        multiversion_bin_version = None
        # Some evergreen task names do not reflect what suite names they run.
        # The suite names should be in the evergreen functions in this case
        if task is None:
            for current_task in evg_conf.tasks:
                func = current_task.find_func_command("run tests") \
                    or current_task.find_func_command("generate resmoke tasks") \
                    or current_task.find_func_command("run benchmark tests")
                if func and get_dict_value(func, ["vars", "suite"]) == suite_name:
                    task = current_task
                    break

                func = current_task.find_func_command("initialize multiversion tasks")
                if not func:
                    continue
                for subtask in func["vars"]:
                    if subtask == suite_name:
                        task = current_task
                        multiversion_bin_version = func["vars"][subtask]
                        break

                if task:
                    break

        if task is None:
            raise RuntimeError(f"Error: Could not find evergreen task definition for {suite_name}")

        is_multiversion = "multiversion" in task.tags
        generate_func = task.find_func_command("generate resmoke tasks")
        is_jstestfuzz = False
        if generate_func:
            is_jstestfuzz = get_dict_value(generate_func, ["vars", "is_jstestfuzz"]) == "true"

        local_args = to_local_args()
        local_args = strip_fuzz_config_params(local_args)
        local_resmoke_invocation = (
            f"{os.path.join('buildscripts', 'resmoke.py')} {' '.join(local_args)}")

        using_config_fuzzer = False
        if config.FUZZ_MONGOD_CONFIGS:
            using_config_fuzzer = True
            local_resmoke_invocation += f" --fuzzMongodConfigs={config.FUZZ_MONGOD_CONFIGS}"

            self._resmoke_logger.info("Fuzzed mongodSetParameters:\n%s",
                                      config.MONGOD_SET_PARAMETERS)
            self._resmoke_logger.info("Fuzzed wiredTigerConnectionString: %s",
                                      config.WT_ENGINE_CONFIG)

        if config.FUZZ_MONGOS_CONFIGS:
            using_config_fuzzer = True
            local_resmoke_invocation += f" --fuzzMongosConfigs={config.FUZZ_MONGOS_CONFIGS}"

            self._resmoke_logger.info("Fuzzed mongosSetParameters:\n%s",
                                      config.MONGOS_SET_PARAMETERS)

        if using_config_fuzzer:
            local_resmoke_invocation += f" --configFuzzSeed={str(config.CONFIG_FUZZ_SEED)}"

        if multiversion_bin_version:
            default_tag_file = config.DEFAULTS["exclude_tags_file_path"]
            local_resmoke_invocation += f" --tagFile={default_tag_file}"

        resmoke_env_options = ''
        if os.path.exists('resmoke_env_options.txt'):
            with open('resmoke_env_options.txt') as fin:
                resmoke_env_options = fin.read().strip()

        local_resmoke_invocation = f"{resmoke_env_options} {local_resmoke_invocation}"
        self._resmoke_logger.info("resmoke.py invocation for local usage: %s",
                                  local_resmoke_invocation)

        lines = []

        if is_multiversion:
            lines.append("# DISCLAIMER:")
            lines.append(
                "#     The `db-contrib-tool` command downloads the latest last-continuous/lts mongo shell binaries available in CI."
            )
            if multiversion_bin_version:
                lines.append(
                    "#     The generated `multiversion_exclude_tags.yml` is dependent on the `backports_required_for_multiversion_tests.yml` file of the last-continuous/lts mongo shell binary git commit."
                )
            lines.append(
                "#     If there have been new commits to last-continuous/lts, the excluded tests & binaries may be slightly different on this task vs locally."
            )
        if is_jstestfuzz:
            lines.append(
                "# This is a jstestfuzz suite and is dependent on the generated tests specific to this task execution."
            )

        if suite.get_description():
            lines.append(f"# {suite.get_description()}")

        lines.append(
            "# Having trouble reproducing your failure with this? Feel free to reach out in #server-testing."
        )
        lines.append("")
        if is_multiversion:
            if not os.path.exists("local-db-contrib-tool-invocation.txt"):
                raise RuntimeError(
                    "ERROR: local-db-contrib-tool-invocation.txt does not exist for multiversion task"
                )

            with open("local-db-contrib-tool-invocation.txt", "r") as fh:
                db_contrib_tool_invocation = fh.read().strip() + " && \\"
                lines.append(db_contrib_tool_invocation)

            if multiversion_bin_version:
                generate_tag_file_invocation = f"buildscripts/resmoke.py generate-multiversion-exclude-tags --oldBinVersion={multiversion_bin_version} && \\"
                lines.append(generate_tag_file_invocation)

        if is_jstestfuzz:
            download_url = f"https://mciuploads.s3.amazonaws.com/{config.EVERGREEN_PROJECT_NAME}/{config.EVERGREEN_VARIANT_NAME}/{config.EVERGREEN_REVISION}/jstestfuzz/{config.EVERGREEN_TASK_ID}-{config.EVERGREEN_EXECUTION}.tgz"
            jstestfuzz_dir = "jstestfuzz/"
            jstests_tar = "jstests.tgz"
            lines.append(f"mkdir -p {jstestfuzz_dir} && \\")
            lines.append(f"rm -rf {jstestfuzz_dir}* && \\")
            lines.append(f"wget '{download_url}' -O {jstests_tar} && \\")
            lines.append(f"tar -xf {jstests_tar} -C {jstestfuzz_dir} && \\")
            lines.append(f"rm {jstests_tar} && \\")

        lines.append(local_resmoke_invocation)

        with open("local-resmoke-invocation.txt", "w") as fh:
            fh.write("\n".join(lines))

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
                if platform.system() == "Darwin":
                    # On macOS, 'psutil.Process.environ' gives non-sensical output if the calling process
                    # does not have permission/entitlement to do so. Refer to the doc for psutil at
                    # https://psutil.readthedocs.io/en/stable/#psutil.Process.environ.
                    # Worst case, it returns the environment variables of a different process,
                    # which may unluckily contain RESMOKE_PARENT_PROCESS. To avoid attempting to kill arbitrary
                    # processes, double-check the environment against the results from 'ps'. Double-check rather
                    # than use ps for all processes because the subprocess handling is slower in the common case
                    # where there are no rogue resmoke processes.
                    cmd = ["ps", "-E", str(proc.pid)]
                    ps_proc = subprocess.run(cmd, capture_output=True)
                    if (f"RESMOKE_PARENT_PROCESS={parent_resmoke_pid}" not in
                            ps_proc.stdout.decode()):
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

    def _log_suite_summary(self, suite: Suite):
        """Log a summary of the suite run."""
        self._resmoke_logger.info("=" * 80)
        self._resmoke_logger.info("Summary of %s suite: %s", suite.get_display_name(),
                                  self._get_suite_summary(suite))

    @TRACER.start_as_current_span("run.__init__._execute_suite")
    def _execute_suite(self, suite: Suite) -> bool:
        """Execute Fa suite and return True if interrupted, False otherwise."""
        execute_suite_span = trace.get_current_span()
        execute_suite_span.set_attributes(attributes=suite.get_suite_otel_attributes())
        self._shuffle_tests(suite)

        # During "docker image build", we are not actually running any tests, so we do not care if there are no tests.
        # Specifically, when building the images for a jstestfuzz external SUT the jstestfuzz test files will not exist yet -- but we still want to
        # build the external SUT infrastructure. The jstestfuzz tests will be generated at runtime via the `_setup_workload_container` method when
        # actually running resmoke from within the workload container against the jstestfuzz external SUT.
        if not config.DOCKER_COMPOSE_BUILD_IMAGES and not suite.tests:
            self._exec_logger.info("Skipping %s, no tests to run", suite.test_kind)
            suite.return_code = 0
            execute_suite_span.set_status(StatusCode.OK)
            execute_suite_span.set_attributes({
                Suite.METRIC_NAMES.RETURN_CODE: suite.return_code,
                Suite.METRIC_NAMES.RETURN_STATUS: "skipped",
            })
            return False
        executor_config = suite.get_executor_config()
        try:
            executor = testing.executor.TestSuiteExecutor(
                self._exec_logger, suite, archive_instance=self._archive, **executor_config)
            # If this is a "docker compose build", we just build the docker compose images for
            # this resmoke configuration and exit.
            if config.DOCKER_COMPOSE_BUILD_IMAGES:
                build_images(suite.get_name(), executor._jobs[0].fixture)
                suite.return_code = 0
            else:
                executor.run()
        except (errors.UserInterrupt, errors.LoggerRuntimeConfigError) as err:
            self._exec_logger.error("Encountered an error when running %ss of suite %s: %s",
                                    suite.test_kind, suite.get_display_name(), err)
            suite.return_code = err.EXIT_CODE
            return_status = "user_interrupt" if isinstance(
                err, errors.UserInterrupt) else "logger_runtime_config"
            execute_suite_span.set_status(StatusCode.ERROR, description=return_status)
            execute_suite_span.set_attributes({
                Suite.METRIC_NAMES.RETURN_CODE: suite.return_code,
                Suite.METRIC_NAMES.RETURN_STATUS: return_status,
            })
            return True
        except OSError as err:
            self._exec_logger.error("Encountered an OSError: %s", err)
            suite.return_code = 74  # Exit code for OSError on POSIX systems.
            return_status = "os_error"
            execute_suite_span.set_status(StatusCode.ERROR, description=return_status)
            execute_suite_span.set_attributes({
                Suite.METRIC_NAMES.RETURN_CODE: suite.return_code,
                Suite.METRIC_NAMES.RETURN_STATUS: return_status,
                Suite.METRIC_NAMES.ERRORNO: err.errno
            })
            return True
        except:  # pylint: disable=bare-except
            self._exec_logger.exception("Encountered an error when running %ss of suite %s.",
                                        suite.test_kind, suite.get_display_name())
            suite.return_code = 2
            return_status = "unknown_error"
            execute_suite_span.set_status(StatusCode.ERROR, description=return_status)
            execute_suite_span.set_attributes({
                Suite.METRIC_NAMES.RETURN_CODE: suite.return_code,
                Suite.METRIC_NAMES.RETURN_STATUS: return_status,
            })
            return False
        execute_suite_span.set_status(StatusCode.OK)
        execute_suite_span.set_attributes({
            Suite.METRIC_NAMES.RETURN_CODE: suite.return_code,
            Suite.METRIC_NAMES.RETURN_STATUS: "success",
        })
        return False

    def _shuffle_tests(self, suite: Suite):
        """Shuffle the tests if the shuffle cli option was set."""
        random.seed(config.RANDOM_SEED)
        if not config.SHUFFLE:
            return
        self._exec_logger.info("Shuffling order of tests for %ss in suite %s. The seed is %d.",
                               suite.test_kind, suite.get_display_name(), config.RANDOM_SEED)
        random.shuffle(suite.tests)

    # pylint: disable=inconsistent-return-statements
    def _get_suites(self) -> List[Suite]:
        """Return the list of suites for this resmoke invocation."""
        try:
            return suitesconfig.get_suites(config.SUITE_FILES, config.TEST_FILES)
        except errors.SuiteNotFound as err:
            self._resmoke_logger.error("Failed to parse YAML suite definition: %s", str(err))
            self.list_suites()
            self.exit(1)
        except errors.InvalidMatrixSuiteError as err:
            self._resmoke_logger.error("Failed to get matrix suite: %s", str(err))
            self.exit(1)
        except errors.TestExcludedFromSuiteError as err:
            self._resmoke_logger.error(
                "Cannot run excluded test in suite config. Use '--force-excluded-tests' to override: %s",
                str(err))
            self.exit(1)
        return []

    def _log_suite_config(self, suite: Suite):
        sb = [
            "YAML configuration of suite {}".format(suite.get_display_name()),
            utils.dump_yaml({"test_kind": suite.get_test_kind_config()}), "",
            utils.dump_yaml({"selector": suite.get_selector_config()}), "",
            utils.dump_yaml({"executor": suite.get_executor_config()}), "",
            utils.dump_yaml({"logging": config.LOGGING_CONFIG})
        ]
        self._resmoke_logger.info("\n".join(sb))

    @staticmethod
    def _get_suite_summary(suite: Suite):
        """Return a summary of the suite run."""
        sb: List[str] = []
        suite.summarize(sb)
        return "\n".join(sb)

    def _setup_signal_handler(self, suites: List[Suite]):
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

    def exit(self, exit_code: int):
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

    RESOURCE_INTENSIVE_TAG = _TagInfo(
        tag_name="resource_intensive",
        evergreen_aware=False,
        suite_options=config.SuiteOptions.ALL_INHERITED._replace(  # type: ignore
            num_jobs=1))

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

        combinations.append(("resource intensive", [(cls.RESOURCE_INTENSIVE_TAG, True)]))
        combinations.append(("not resource intensive", [(cls.RESOURCE_INTENSIVE_TAG, False)]))

        return combinations

    def _get_suites(self) -> List[Suite]:
        """Return a list of resmokelib.testing.suite.Suite instances to execute.

        For every resmokelib.testing.suite.Suite instance returned by resmoke.Main._get_suites(),
        multiple copies of that test suite are run using different resmokelib.config.SuiteOptions()
        depending on whether each tag in the combination is enabled or not.
        """

        suites = []

        for suite in TestRunner._get_suites(self):
            if suite.test_kind not in ("js_test", "fsm_workload_test"):
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
        RunPlugin._add_generate(subparsers)
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
                          'generate-multiversion-exclude-tags', 'generate-matrix-suites'):
            configure_resmoke.validate_and_update_config(parser, parsed_args)
            if config.EVERGREEN_TASK_ID is not None:
                return TestRunnerEvg(subcommand, **kwargs)
            else:
                return TestRunner(subcommand, **kwargs)
        return None

    @classmethod
    def _add_run(cls, subparsers: argparse._SubParsersAction):
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

        parser.add_argument(
            "--force-excluded-tests", dest="force_excluded_tests", action="store_true",
            help=("Allows running tests in a suite config's excluded test roots"
                  " when passed as positional arg(s)."))

        parser.add_argument(
            "--skipSymbolization",
            dest="skip_symbolization",
            action="store_true",
            help="Skips symbolizing stacktraces generated by tests.",
        )

        parser.add_argument("--genny", dest="genny_executable", metavar="PATH",
                            help="The path to the genny executable for resmoke to use.")

        parser.add_argument(
            "--includeWithAnyTags", action="append", dest="include_with_any_tags",
            metavar="TAG1,TAG2",
            help=("Comma separated list of tags. For the jstest portion of the suite(s),"
                  " only tests which have at least one of the specified tags will be"
                  " run."))

        parser.add_argument(
            "--dockerComposeBuildImages", dest="docker_compose_build_images",
            metavar="IMAGE1,IMAGE2,IMAGE3", help=
            ("Comma separated list of base images to build for running resmoke against an External System Under Test:"
             " (1) `workload`: Your mongo repo with a python development environment setup."
             " (2) `mongo-binaries`: The `mongo`, `mongod`, `mongos` binaries to run tests with."
             " (3) `config`: The target suite's `docker-compose.yml` file, startup scripts & configuration."
             " All three images are needed to successfully setup an External System Under Test."
             " This will not run any tests. It will just build the images and generate"
             " the `docker-compose.yml` configuration to set up the External System Under Test for the desired suite."
             ))

        parser.add_argument(
            "--dockerComposeBuildEnv", dest="docker_compose_build_env",
            choices=["local", "evergreen"], default="local", help=
            ("Set the environment where this `--dockerComposeBuildImages` is happening -- defaults to: `local`."
             ))

        parser.add_argument(
            "--dockerComposeTag", dest="docker_compose_tag", metavar="TAG", default="development",
            help=("The `tag` name to use for images built during a `--dockerComposeBuildImages`."))

        parser.add_argument(
            "--externalSUT", dest="external_sut", action="store_true", default=False, help=
            ("This option should only be used when running resmoke against an External System Under Test."
             " The External System Under Test should be setup via the command generated after"
             " running: `buildscripts/resmoke.py run --suite [suite_name] ... --dockerComposeBuildImages"
             " config,workload,mongo-binaries`."))

        parser.add_argument(
            "--sanityCheck", action="store_true", dest="sanity_check", help=
            "Truncate the test queue to 1 item, just in order to verify the suite is properly set up."
        )

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
            "--shellSeed", action="store", dest="shell_seed", default=None,
            help=("Sets the seed for replset and sharding fixtures to use. "
                  "This only works when only one test is input into resmoke."))

        parser.add_argument(
            "--mongocryptdSetParameters", dest="mongocryptd_set_parameters", action="append",
            metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
            help=("Passes one or more --setParameter options to all mongocryptd processes"
                  " started by resmoke.py. The argument is specified as bracketed YAML -"
                  " i.e. JSON with support for single quoted and unquoted keys."))

        parser.add_argument("--numClientsPerFixture", type=int, dest="num_clients_per_fixture",
                            help="Number of clients running tests per fixture.")

        parser.add_argument(
            "--useTenantClient", default=False, dest="use_tenant_client", action="store_true", help=
            "Use tenant client. If set, each client will be constructed with a generated tenant id."
        )

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

        parser.add_argument("--shellGRPC", dest="shell_grpc", action="store_true",
                            help="Whether to use gRPC by default when connecting via the shell.")

        parser.add_argument("--shellTls", dest="shell_tls_enabled", action="store_true",
                            help="Whether to use TLS when connecting.")

        parser.add_argument("--shellTlsCertificateKeyFile", dest="shell_tls_certificate_key_file",
                            metavar="SHELL_TLS_CERTIFICATE_KEY_FILE",
                            help="The TLS certificate to use when connecting.")

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
            "--runNoFeatureFlagTests", dest="run_no_feature_flag_tests", action="store_true",
            help=("Do not run any tests tagged with enabled feature flags."
                  " This argument has precedence over --runAllFeatureFlagTests"
                  "; used for multiversion suites"))

        parser.add_argument("--additionalFeatureFlags", dest="additional_feature_flags",
                            action="append", metavar="featureFlag1, featureFlag2, ...",
                            help="Additional feature flags")

        parser.add_argument("--additionalFeatureFlagsFile", dest="additional_feature_flags_file",
                            action="store", metavar="FILE",
                            help="The path to a file with feature flags, delimited by newlines.")

        parser.add_argument("--maxTestQueueSize", type=int, dest="max_test_queue_size",
                            help=argparse.SUPPRESS)

        parser.add_argument("--tagFile", action="append", dest="tag_files", metavar="TAG_FILES",
                            help="One or more YAML files that associate tests and tags.")

        configure_resmoke.add_otel_args(parser)

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

        mongodb_server_options.add_argument(
            "--enableEnterpriseTests", action="store", dest="enable_enterprise_tests", default="on",
            choices=("on", "off"), metavar="ON|OFF",
            help=("Enable or disable enterprise tests. Defaults to 'on'."))

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
            "--tlsMode", dest="tls_mode", metavar="TLS_MODE", help="Indicates what TLS mode mongod "
            "and mongos servers should be started with. See also: https://www.mongodb.com"
            "/docs/manual/reference/configuration-options/#mongodb-setting-net.tls.mode")

        mongodb_server_options.add_argument(
            "--tlsCAFile", dest="tls_ca_file", metavar="TLS_CA_FILE",
            help="Path to the CA certificate file to be used by all clients and servers.")

        mongodb_server_options.add_argument(
            "--mongodTlsCertificateKeyFile", dest="mongod_tls_certificate_key_file",
            metavar="MONGOD_TLS_CERTIFICATE_KEY_FILE",
            help="Path to the TLS certificate to be used by all mongods.")

        mongodb_server_options.add_argument(
            "--mongosTlsCertificateKeyFile", dest="mongos_tls_certificate_key_file",
            metavar="MONGOS_TLS_CERTIFICATE_KEY_FILE",
            help="Path to the TLS certificate to be used by all mongoses.")

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

        mongodb_server_options.add_argument(
            "--fuzzMongodConfigs", dest="fuzz_mongod_configs",
            help="Randomly chooses mongod parameters that were not specified. Use 'stress' to fuzz "
            "all configs including stressful storage configurations that may significantly "
            "slow down the server. Use 'normal' to only fuzz non-stressful configurations. ",
            metavar="MODE", choices=('normal', 'stress'))

        mongodb_server_options.add_argument(
            "--fuzzMongosConfigs", dest="fuzz_mongos_configs",
            help="Randomly chooses mongos parameters that were not specified", metavar="MODE",
            choices=('normal', ))

        mongodb_server_options.add_argument(
            "--configFuzzSeed", dest="config_fuzz_seed", metavar="PATH",
            help="Sets the seed used by mongod and mongos config fuzzers")

        mongodb_server_options.add_argument(
            "--configShard", dest="config_shard", metavar="CONFIG",
            help="If set, specifies which node is the config shard. Can also be set to 'any'.")

        mongodb_server_options.add_argument(
            "--embeddedRouter", dest="embedded_router", metavar="CONFIG",
            help="If set, uses embedded routers instead of dedicated mongos.")

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

        internal_options.add_argument("--cedarReportFile", dest="cedar_report_file",
                                      metavar="CEDAR_REPORT",
                                      help="Writes a JSON file with performance test results.")

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

        evergreen_options.add_argument("--taskWorkDir", dest="work_dir", metavar="TASK_WORK_DIR",
                                       help="Sets the working directory of the task.")

        evergreen_options.add_argument(
            "--projectConfigPath", dest="evg_project_config_path",
            help="Sets the path to evergreen project configuration yaml.")

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
    def _add_list_suites(cls, subparsers: argparse._SubParsersAction):
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
    def _add_generate(cls, subparsers: argparse._SubParsersAction):
        """Create and add the parser for the generate subcommand."""
        subparsers.add_parser("generate-matrix-suites",
                              help="Generate matrix suite config files from the mapping files.")

    @classmethod
    def _add_find_suites(cls, subparsers: argparse._SubParsersAction):
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
    def _add_list_tags(cls, subparsers: argparse._SubParsersAction):
        """Create and add the parser for the list-tags subcommand."""
        parser = subparsers.add_parser(
            "list-tags", help="Lists the tags and their documentation available in the suites.")
        parser.set_defaults(logger_file="console")
        parser.add_argument(
            "--suites", dest="suite_files", metavar="SUITE1,SUITE2",
            help=("Comma separated list of suite names to get tags from."
                  " All suites are used if unspecified."))

    @classmethod
    def _add_generate_multiversion_exclude_tags(cls, subparser: argparse._SubParsersAction):
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


def to_local_args(input_args: Optional[List[str]] = None):
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

    # The top-level parser has one subparser that contains all subcommand parsers.
    command_subparser = [
        action for action in parser._actions  # pylint: disable=protected-access
        if action.dest == "command"
    ][0]

    run_parser = command_subparser.choices.get("run")

    # arguments that are in the standard run parser that we do not want to include in the local invocation
    skipped_args = ["install_dir", "tag_files"]

    suites_arg = None
    storage_engine_arg = None
    other_local_args = []
    positional_args = []

    def format_option(option_name, option_value):
        """
        Return <option_name>=<option_value>.

        This function assumes that 'option_name' is always "--" prefix and isn't "-" prefixed.
        """
        if " " not in str(option_value):
            return f"{option_name}={option_value}"
        else:
            return f"'{option_name}={option_value}'"

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
            elif arg_dest in skipped_args:
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


def strip_fuzz_config_params(input_args: List[str]):
    """Delete fuzz related command line args because we have to add the seed manually."""

    ret = []
    for arg in input_args:
        if not arg.startswith(("--fuzzMongodConfigs", "--fuzzMongosConfigs", "--configFuzzSeed")):
            ret.append(arg)

    return ret
