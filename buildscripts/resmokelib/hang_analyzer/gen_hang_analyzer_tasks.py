#!/usr/bin/env python3
"""Generate a task to run core analysis on uploaded core dumps in evergreen."""

import argparse
import glob
import json
import os
import pathlib
import random
import re
import string
import sys
from abc import ABC, abstractmethod
from typing import List, NamedTuple, Optional
from unittest.mock import MagicMock

from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskDependency
from shrub.v2.command import BuiltInCommand

from buildscripts.resmokelib.core.process import BORING_CORE_DUMP_PIDS_FILE
from buildscripts.resmokelib.hang_analyzer import dumper
from evergreen.task import Task as EvgTask

mongo_path = pathlib.Path(__file__).parents[3]
sys.path.append(mongo_path)

from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.util.fileops import write_file
from buildscripts.util.read_config import read_config_file

GENERATED_TASK_PREFIX = "core_analysis"
RANDOM_STRING_LENGTH = 5
LOCAL_BIN_DIR = os.path.join("dist-test", "bin")
MULTIVERSION_BIN_DIR = os.path.normpath("/data/multiversion")


class CoreInfo(NamedTuple):
    path: str
    binary_name: str
    pid: str
    marked_boring: bool


class CoreAnalysisTaskGenerator(ABC):
    @abstractmethod
    def get_core_analyzer_commands(
        self,
        task_id: str,
        execution: str,
        core_analyzer_results_url: str,
        gdb_index_cache: str,
        has_interesting_core_dumps: bool,
        boring_core_dump_pids: set,
    ) -> List[FunctionCall]:
        pass

    @abstractmethod
    def find_cores(self) -> list[CoreInfo]:
        pass

    def get_core_analysis_task_dependencies(self, compile_variant: str) -> set[TaskDependency]:
        return []

    def __init__(
        self,
        expansions_file: str = "../expansions.yml",
        use_mock_tasks: bool = False,
    ):
        self.expansions = read_config_file(expansions_file)

        if use_mock_tasks:
            task = MagicMock()
            task.display_name = "resmoke_tests"
            task.id = "resmoke_tests_task_id_123"
            build = MagicMock()
            build.get_tasks.return_value = [task]
            self.evg_api = MagicMock()
            self.evg_api.build_by_id.return_value = build
        else:
            try:
                self.evg_api = evergreen_conn.get_evergreen_api()
            except RuntimeError:
                print(
                    "WARNING: Cannot generate core analysis because the evergreen api file could not be found.",
                    file=sys.stderr,
                )
                print(
                    "This is probably not an error, if you want core analysis to run on this task make sure",
                    file=sys.stderr,
                )
                print(
                    "the evergreen function 'configure evergreen api credentials' is called before this task",
                    file=sys.stderr,
                )
                return None

    def generate(self) -> Optional[dict]:
        if not sys.platform.startswith("linux"):
            print("This platform is not supported, skipping core analysis task generation.")
            return None

        # gather information from the current task being run
        distro = None
        for distro_expansion in ["core_analyzer_distro_name", "large_distro_name", "distro_id"]:
            if distro := self.expansions.get(distro_expansion, None):
                break
        assert distro is not None
        current_task_name = self.expansions.get("task_name")
        task_id = self.expansions.get("task_id")
        execution = self.expansions.get("execution")
        gdb_index_cache = (
            "off" if self.expansions.get("core_analyzer_gdb_index_cache") == "off" else "on"
        )
        build_variant_name = self.expansions.get("build_variant")
        core_analyzer_results_url = self.expansions.get("core_analyzer_results_url")
        compile_variant = self.expansions.get("compile_variant")

        task_info = self.evg_api.task_by_id(task_id)

        skip_variants = ["commit-queue"]
        if task_info.build_variant in skip_variants:
            print(f"Skipping core analysis task generation for variant: {task_info.build_variant}")
            return None

        # make sure we are not creating an infinite loop by generating a task from another generated task
        if current_task_name.startswith(GENERATED_TASK_PREFIX):
            print(
                f"Skipping task generation because {current_task_name} starts with {GENERATED_TASK_PREFIX}"
            )
            return None

        cores = self.find_cores()
        boring_cores = [core for core in cores if core.marked_boring]
        interesting_cores = [core for core in cores if not core.marked_boring]
        boring_core_dump_pids = set([core.pid for core in boring_cores])

        if not cores:
            print("No core dumps found.")
            return None

        print(f"Detected core dumps: {[core.path for core in cores]}")
        print(f"Core dumps marked as boring by resmoke: {[core.path for core in boring_cores]}")

        if not interesting_cores:
            print(
                "No interesting core dumps were found. Not activating the core analysis task. It is still generated, but must be manually activated."
            )
        should_activate = len(interesting_cores) > 0 and not self._should_skip_task(task_info)

        build_variant = BuildVariant(name=build_variant_name)
        commands = self.get_core_analyzer_commands(
            task_id,
            execution,
            core_analyzer_results_url,
            gdb_index_cache,
            should_activate,
            boring_core_dump_pids,
        )

        deps = self.get_core_analysis_task_dependencies(compile_variant)

        sub_tasks = set(
            [Task(get_generated_task_name(current_task_name, execution), commands, deps)]
        )

        build_variant.add_tasks(sub_tasks, distros=[distro], activate=should_activate)

        shrub_project = ShrubProject.empty()
        shrub_project.add_build_variant(build_variant)

        # shrub.py currently does not support adding task deps that override the variant deps
        output_dict = shrub_project.as_dict()
        deps_list = []
        for dep in deps:
            deps_list.append(dep.as_dict())
        for variant in output_dict["buildvariants"]:
            for task in variant["tasks"]:
                task["depends_on"] = deps_list

        return output_dict

    def _should_skip_task(self, task: EvgTask) -> bool:
        # We hardcode some task names where the core analysis is extending the long pole
        # of required patch builds by 100 mins and the BFs are taking too long to fix.
        # This list is a quick fix to improve development velocity.
        # TODO(SERVER-118661): Remove disagg suites from skip list.
        skip_tasks = [
            "disagg_repl_jscore_passthrough",
            "disagg_repl_jscore_passthrough_secondary_reads",
            "disagg_sharded_colls_jscore_passthrough_secondary_reads_with_balancer",
            "disagg_two_nodes_repl_jscore_passthrough",
            "no_passthrough_disagg_override",
        ]

        current_task_name = task.display_name
        if task.parent_task_id:
            parent_task = self.evg_api.task_by_id(task.parent_task_id)
            current_task_name = parent_task.display_name
        if current_task_name in skip_tasks:
            print(f"Not activating core analysis task for task: {current_task_name}")
            return True

        return False


class ResmokeCoreAnalysisTaskGenerator(CoreAnalysisTaskGenerator):
    def get_core_analyzer_commands(
        self,
        task_id: str,
        execution: str,
        core_analyzer_results_url: str,
        gdb_index_cache: str,
        has_interesting_core_dumps: bool,
        boring_core_dump_pids: set,
    ) -> List[FunctionCall]:
        return _get_core_analyzer_commands(
            task_id,
            execution,
            core_analyzer_results_url,
            gdb_index_cache,
            has_interesting_core_dumps,
            boring_core_dump_pids,
        )

    def get_core_analysis_task_dependencies(self, compile_variant: str) -> set[TaskDependency]:
        # TODO SERVER-92571 add archive_jstestshell_debug dep for variants that have it.
        return {TaskDependency("archive_dist_test_debug", compile_variant)}

    def find_cores(self) -> list[CoreInfo]:
        cores = []

        # LOCAL_BIN_DIR does not exists on non-resmoke tasks, so return early as there is no work to be done.
        if not os.path.exists(LOCAL_BIN_DIR):
            print(f"Skipping task generation because binary directory not found: {LOCAL_BIN_DIR}")
            return cores

        # Get boring core dump PIDs to pass to the analyzer
        boring_core_dump_pids = set()
        if os.path.exists(BORING_CORE_DUMP_PIDS_FILE):
            with open(BORING_CORE_DUMP_PIDS_FILE, "r") as file:
                boring_core_dump_pids = set(file.read().split())

        task_id = self.expansions.get("task_id")
        task_info = self.evg_api.task_by_id(task_id)
        dumpers = dumper.get_dumpers(None, None)

        for artifact in task_info.artifacts:
            regex = re.search(r"Core Dump [0-9]+ \((.*)\.gz\)", artifact.name)
            if not regex:
                continue

            core_file = regex.group(1)
            binary_name, bin_version = dumpers.dbg.get_binary_from_core_dump(core_file)
            dir_to_check = MULTIVERSION_BIN_DIR if bin_version else LOCAL_BIN_DIR
            binary_files = os.listdir(dir_to_check)
            if binary_name not in binary_files:
                print(f"{core_file} was generated by {binary_name} but the binary was not found.")
                continue

            pid = get_core_pid(core_file)
            boring = pid in boring_core_dump_pids

            cores.append(
                CoreInfo(path=core_file, binary_name=binary_name, marked_boring=boring, pid=pid)
            )
        return cores


class BazelCoreAnalysisTaskGenerator(CoreAnalysisTaskGenerator):
    def get_core_analyzer_commands(
        self,
        task_id: str,
        execution: str,
        core_analyzer_results_url: str,
        gdb_index_cache: str,
        has_interesting_core_dumps: bool,
        boring_core_dump_pids: set,
    ) -> List[FunctionCall]:
        return _get_core_analyzer_commands(
            task_id,
            execution,
            core_analyzer_results_url,
            gdb_index_cache,
            has_interesting_core_dumps,
            boring_core_dump_pids,
            is_bazel_task=True,
        )

    def find_cores(self) -> list[CoreInfo]:
        cores = []
        results_dir = os.path.join(self.expansions.get("workdir"), "results")
        if not os.path.exists(results_dir):
            return cores

        # Search for core files in results/**/test.outputs/ directories
        results_dirs = glob.glob(os.path.join(results_dir, "**", "test.outputs"), recursive=True)
        for dir in results_dirs:
            boring_dump_file = os.path.join(dir, BORING_CORE_DUMP_PIDS_FILE)
            if os.path.exists(boring_dump_file):
                with open(BORING_CORE_DUMP_PIDS_FILE, "r") as file:
                    boring_core_dump_pids = set(file.read().split())
            else:
                boring_core_dump_pids = {}

            core_patterns = [
                os.path.join(dir, "*.core"),
                os.path.join(dir, "*.mdmp"),
            ]
            for pattern in core_patterns:
                for core in glob.glob(pattern, recursive=True):
                    # Check if resmoke reported this core dump as a "boring one", in the BORING_CORE_DUMP_PIDS_FILE.
                    pid = get_core_pid(os.path.basename(core))
                    boring = pid in boring_core_dump_pids

                    cores.append(CoreInfo(path=core, binary_name="", marked_boring=boring, pid=pid))
        return cores


def get_generated_task_name(current_task_name: str, execution: str) -> str:
    # random string so we do not define the same task name for multiple variants which causes issues
    random_string = "".join(
        random.choices(
            string.ascii_uppercase + string.digits + string.ascii_lowercase, k=RANDOM_STRING_LENGTH
        )
    )
    return f"{GENERATED_TASK_PREFIX}_{current_task_name}{execution}_{random_string}"


def get_core_pid(core_file_name: str) -> int:
    # Expected format is like dump_mongod.429814.core or dump_mongod-8.2.429814.core, where 429814 is the PID.
    parts = core_file_name.split(".")
    assert len(parts) >= 3, "Unknown core dump file name format"
    assert str.isdigit(parts[-2]), "PID not in expected location of core dump file name"
    pid = parts[-2]
    return pid


def _get_core_analyzer_commands(
    task_id: str,
    execution: str,
    core_analyzer_results_url: str,
    gdb_index_cache: str,
    has_interesting_core_dumps: bool,
    boring_core_dump_pids: set,
    is_bazel_task: bool = False,
) -> List[FunctionCall]:
    """Return setup commands."""
    return [
        FunctionCall("f_expansions_write"),
        BuiltInCommand("manifest.load", {}),
        FunctionCall("git get project and add git tag"),
        FunctionCall("f_expansions_write"),
        FunctionCall("kill processes"),
        FunctionCall("cleanup environment"),
        FunctionCall("set up venv"),
        FunctionCall("upload pip requirements"),
        FunctionCall("configure evergreen api credentials"),
        BuiltInCommand(
            "subprocess.exec",
            {
                "binary": "bash",
                "args": [
                    "src/evergreen/run_python_script.sh",
                    "buildscripts/resmoke.py",
                    "core-analyzer",
                    f"--task-id={task_id}",
                    f"--execution={execution}",
                    f"--gdb-index-cache={gdb_index_cache}",
                    f"--boring-core-dump-pids={','.join(boring_core_dump_pids)}",
                    "--generate-report",
                    f"--otel-extra-data=has_interesting_core_dumps={str(has_interesting_core_dumps).lower()}",
                ]
                + ["--is-bazel-task" if is_bazel_task else None],
                "env": {
                    "OTEL_TRACE_ID": "${otel_trace_id}",
                    "OTEL_PARENT_ID": "${otel_parent_id}",
                    "OTEL_COLLECTOR_DIR": "../build/OTelTraces/",
                },
            },
        ),
        BuiltInCommand(
            "archive.targz_pack",
            {
                "target": "src/mongo-coreanalysis.tgz",
                "source_dir": "src",
                "include": ["./core-analyzer/analysis/**"],
            },
        ),
        BuiltInCommand(
            "s3.put",
            {
                "aws_key": "${aws_key}",
                "aws_secret": "${aws_secret}",
                "local_file": "src/mongo-coreanalysis.tgz",
                "remote_file": core_analyzer_results_url,
                "bucket": "mciuploads",
                "permissions": "public-read",
                "content_type": "application/gzip",
                "display_name": "Core Analyzer Output - Execution ${execution}",
            },
        ),
        # We delete the core dumps after we are done processing them so they are not
        # reuploaded to s3 in the generated task's post task block
        FunctionCall(
            "remove files",
            {
                "files": " ".join(
                    ["src/core-analyzer/core-dumps/*.core", "src/core-analyzer/core-dumps/*.mdmp"]
                )
            },
        ),
    ]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--expansions-file",
        help="Location of evergreen expansions file.",
        default="../expansions.yml",
    )
    parser.add_argument(
        "--output-file",
        help="Name of output file to write the generated task config to.",
        default="hang_analyzer_task.json",
    )
    parser.add_argument(
        "--tests-use-bazel",
        action="store_true",
        help="Generate for bazel result task (look for cores in results/*/test.outputs/)",
    )
    parser.add_argument(
        "--use-mock-tasks",
        action="store_true",
        help=argparse.SUPPRESS,  # Use mock Evergreen tasks and skip Evergreen API calls, for unit testing this script.
    )
    args = parser.parse_args()

    if args.tests_use_bazel:
        generator = BazelCoreAnalysisTaskGenerator(args.expansions_file, args.use_mock_tasks)
    else:
        generator = ResmokeCoreAnalysisTaskGenerator(args.expansions_file, args.use_mock_tasks)

    task_config = generator.generate()
    if task_config:
        write_file(args.output_file, json.dumps(task_config, indent=4))
