#!/usr/bin/env python3
#
# Component/Team Smoke Tests
#
# To be run prior to submitting evergreen patches.
# Runs the following locally and makes sure they pass:
#   * clang format
#   * clang tidy
#   * build install-dist-test
#   * <component/team> unit tests
#   * <component/team> smoke tests
#
# By default, notifies the locally configured Evergreen user
# via slack once the smoke test are finished.
#

import hashlib
import os
import shutil
import subprocess
import sys
import time
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from socket import gethostname
from typing import Any, Deque, Dict, List, Optional, Set, Union

from rich.status import Status

SMOKE_TEST_DIR = Path(__file__).resolve().parent
ROOT = SMOKE_TEST_DIR.parent.parent
MONGO_PYTHON = ROOT.joinpath("python3-venv")
MONGO_PYTHON_INTERPRETER = MONGO_PYTHON.joinpath("bin", "python")
BAZEL = Path(shutil.which("bazel"))


def make_unique_name():
    ctx = hashlib.new("sha256")
    ctx.update(ROOT.resolve().as_posix().encode())
    return ctx.hexdigest()[-8:]


REPO_UNIQUE_NAME = make_unique_name()


def ensure_python3_venv():
    if sys.executable != MONGO_PYTHON_INTERPRETER.as_posix():
        os.execv(
            MONGO_PYTHON_INTERPRETER,
            [MONGO_PYTHON_INTERPRETER, *sys.argv],
        )
    # needed for relative imports for eg: buildscripts
    sys.path.append(ROOT.as_posix())


ensure_python3_venv()
# can import these after verifying we're running with the correct venv
from buildscripts.resmokelib.utils.evergreen_conn import get_evergreen_api


@dataclass
class Node:
    name: str
    args: List[str]
    popen_kwargs: Dict[str, str]
    log_file: Path
    deps: Set["Node"]
    _start_time: Optional[float] = None
    _finish_time: Optional[float] = None
    _proc: Optional[subprocess.Popen] = None

    def __str__(self):
        return f'Node("{self.name}")'

    def __repr__(self):
        return f'Node("{self.name}")'

    def __hash__(self):
        return hash(self.name)

    def __eq__(self, other: "Node"):
        return self.name == other.name

    def __lt__(self, other: "Node"):
        return self.name < other.name

    def start(self):
        self._start_time = time.monotonic()
        logstream = self.log_file.open("w")
        self._proc = subprocess.Popen(
            self.args,
            stdout=logstream,
            stderr=logstream,
        )

    def returncode(self):
        if self._proc is None:
            return None
        if self._finish_time is not None:
            return self._proc.returncode
        if self._proc.poll() is None:
            return None
        self._finish_time = time.monotonic()
        return self._proc.returncode

    def deps_are_satisfied(self, finished: Set["Node"]):
        return self.deps.issubset(finished)


def normalize_deps(x: Union[None, Node, Set[Node]]):
    if x is None:
        return set()
    elif isinstance(x, (tuple, list, set)):
        return set(x)
    else:
        return {x}


def send_slack_notification(nodes: List[Node], total_elapsed: float, component_name: str):
    overall_success = True
    lines = [
        "```",
        f"id={REPO_UNIQUE_NAME} host={gethostname()} root={ROOT}",
    ]

    failure_lines = list()

    for node in nodes:
        rc = node.returncode()
        succeeded = rc == 0
        finished = rc is not None
        command = " ".join(node.args)
        overall_success &= succeeded
        if succeeded:
            elapsed = node._finish_time - node._start_time
            lines.append(f"{time.strftime('%H:%M:%S', time.gmtime(elapsed))} {node.name} ✔")
        elif not finished:
            lines.append(f"            {node.name}")
        else:
            elapsed = node._finish_time - node._start_time
            lines.append(f"{time.strftime('%H:%M:%S', time.gmtime(elapsed))} {node.name} ✖")
            failure_lines.append(f"Command '{node.name}', rc={node._proc.returncode}:")
            failure_lines.append(f"```\n{command}\n```")
            failure_lines.append(f"Log: {node.log_file}")

    lines.append("```")
    lines.extend(failure_lines)

    if overall_success:
        lines.insert(
            0,
            f"SUCCESS - {component_name} smoke tests passed in {time.strftime('%H:%M:%S', time.gmtime(total_elapsed))}",
        )
    else:
        lines.insert(
            0,
            f"FAILURE - {component_name} smoke tests failed in {time.strftime('%H:%M:%S', time.gmtime(total_elapsed))}",
        )

    evg = get_evergreen_api()
    evg.send_slack_message(
        target=f"@{evg._auth.username}",
        msg="\n".join(lines),
    )


class CommandRunner:
    def __init__(
        self,
        *,
        log_path: Path,
        notify_slack: bool,
        parallelism: int,
        component_name: str,
    ):
        self._log_path = log_path
        self._parallelism = parallelism
        self._downstream: Dict[Node, Set[Node]] = defaultdict(set)
        self._nodes: Set[Node] = set()
        self._finished: Set[Node] = set()
        self._ready: Deque[Node] = deque()
        self._running: Set[Node] = set()
        self._status = Status(status=f"{component_name} smoke tests")
        self._start_time = time.monotonic()
        self._finish_time: Optional[float] = None
        self._notify_slack = notify_slack
        self._component_name = component_name

    def _notify(self, event: str, node: Node):
        if event == "spawn":
            print(f"{' '.join(node.args)}")
        self._update_display()

    def _update_display(self):
        nrun = len(self._running)
        nfin = len(self._finished)
        ntot = len(self._nodes)
        elapsed = time.monotonic() - self._start_time
        self._status.update(
            status=f"running {nrun}, completed {nfin}/{ntot} {time.strftime('%H:%M:%S', time.gmtime(elapsed))}"
        )

    def command(
        self,
        *,
        name: str,
        args: List[Any],
        log_file: str,
        deps: Union[None, Node, Set[Node]] = None,
        **kwargs,
    ) -> Node:
        log_file = self._log_path.joinpath(log_file)
        kwargs.setdefault("cwd", ROOT)

        node = Node(
            name=name,
            args=list(map(str, args)),
            popen_kwargs=kwargs,
            log_file=log_file,
            deps=normalize_deps(deps),
        )
        self._nodes.add(node)
        if len(node.deps) == 0:
            self._notify("ready", node)
            self._ready.append(node)

        for dep in node.deps:
            self._downstream[dep].add(node)

        return node

    def run(self):
        print(f"Logging results to {self._log_path}")
        self._status.start()
        try:
            iter_finished: Set[Node] = set()
            while self._finished != self._nodes:
                while len(self._running) < self._parallelism and len(self._ready) > 0:
                    node = self._ready.popleft()
                    node.start()
                    self._running.add(node)
                    self._notify("spawn", node)

                for node in self._running:
                    rc = node.returncode()
                    if rc is not None:
                        self._notify("reap", node)
                        iter_finished.add(node)
                        self._finished.add(node)
                        if rc != 0:
                            raise subprocess.CalledProcessError(
                                returncode=rc,
                                cmd=" ".join(node.args),
                                output=f"Log: {node.log_file}",
                            )
                        for down in self._downstream[node]:
                            if down.deps_are_satisfied(self._finished):
                                self._notify("ready", down)
                                self._ready.append(down)
                for node in iter_finished:
                    self._running.remove(node)
                if len(iter_finished) == 0:
                    time.sleep(0.1)
                iter_finished.clear()
                self._update_display()

            elapsed = time.monotonic() - self._start_time
            if self._notify_slack:
                send_slack_notification(
                    nodes=sorted(self._nodes),
                    total_elapsed=elapsed,
                    component_name=self._component_name,
                )
            print(f"Completed {len(self._finished)}/{len(self._nodes)} in {elapsed:.3f}s.")
        except subprocess.CalledProcessError as cpe:
            print(f"""\
Failure:
    command {cpe.cmd}
    rc      {cpe.returncode}
    log     {cpe.output}""")
            send_slack_notification(
                nodes=sorted(self._nodes),
                total_elapsed=time.monotonic() - self._start_time,
                component_name=self._component_name,
            )
            raise
        finally:
            self._status.stop()


component_name_to_formal_name = {
    "catalog-and-routing": "Catalog and Routing",
    "replication": "Replication",
    "server-integration": "Storage Engines Server Integration",
    "server-programmability": "Server Programmability",
    "storage-execution": "Storage Execution",
    "server-bsoncolumn": "Server BSONColumn",
    "server-collection-write-path": "Server Collection Write Path",
    "server-external-sorter": "Server External Sorter",
    "server-index-builds": "Server Index Builds",
    "server-key-string": "Server Key String",
    "server-storage-engine-integration": "Server Storage Engine Integration",
    "server-timeseries-bucket-catalog": "Server Timeseries Bucket Catalog",
    "server-ttl": "Server TTL",
}


component_name_to_integration_test_suite = {
    "catalog-and-routing": "catalog_and_routing",
    "replication": "replication",
    "storage-execution": "storage_execution",
    "server-integration": "server_integration",
    "server-collection-write-path": "server_collection_write_path",
    "server-index-builds": "server_index_builds",
    "server-programmability": "server_programmability",
    "server-storage-engine-integration": "server_storage_engine_integration",
    "server-ttl": "server_ttl",
}

component_name_to_unit_test_tag = {
    "server-integration": "server-integration-smoke",
    "replication": "mongo_unittest",
    "storage-execution": "server-bsoncolumn,server-collection-write-path,server-external-sorter,server-index-builds,server-key-string,server-storage-engine-integration,server-timeseries-bucket-catalog,server-tracking-allocators,server-ttl",
    "server-bsoncolumn": "server-bsoncolumn",
    "server-collection-write-path": "server-collection-write-path",
    "server-external-sorter": "server-external-sorter",
    "server-index-builds": "server-index-builds",
    "server-key-string": "server-key-string",
    "server-storage-engine-integration": "server-storage-engine-integration",
    "server-timeseries-bucket-catalog": "server-timeseries-bucket-catalog",
    "server-tracking-allocators": "server-tracking-allocators",
    "server-ttl": "server-ttl",
}


def run_smoke_tests(
    *,
    component_name,
    log_path: Path,
    upstream_branch: str,
    bazel_args: List[str],
    run_clang_tidy: bool,
    send_slack_notification: bool,
):
    log_path = log_path.joinpath(REPO_UNIQUE_NAME)
    log_path.mkdir(parents=True, exist_ok=True)

    integration_suite_name = (
        component_name_to_integration_test_suite[component_name]
        if component_name in component_name_to_integration_test_suite
        else ""
    )

    unit_test_tag = (
        component_name_to_unit_test_tag[component_name]
        if component_name in component_name_to_unit_test_tag
        else ""
    )

    unit_test_build_target = (
        "//src/mongo/db/repl/..." if component_name == "replication" else "//src/mongo/..."
    )

    component_name_for_messages = component_name_to_formal_name[component_name]

    runner = CommandRunner(
        component_name=component_name_for_messages,
        log_path=log_path,
        notify_slack=send_slack_notification,
        parallelism=os.cpu_count(),
    )

    formatters = [
        runner.command(
            name="clang format",
            args=[
                MONGO_PYTHON_INTERPRETER,
                ROOT.joinpath("buildscripts", "clang_format.py"),
                "format-my",
                upstream_branch,
            ],
            log_file="clang_format.log",
        ),
        runner.command(
            name="misc. lint",
            args=[
                BAZEL,
                "run",
                "//:lint",
            ],
            log_file="misc_lint.log",
        ),
        runner.command(
            # catch-all for other bazel-driven formatters
            name="misc. format",
            args=[
                BAZEL,
                "run",
                "//:format",
            ],
            log_file="misc_format.log",
        ),
    ]

    install = runner.command(
        name="build install executables",
        args=[
            BAZEL,
            "build",
            *bazel_args,
            "//:install-dist-test",
        ],
        log_file="build_install_dist_test.log",
        deps=formatters,
    )

    integration_tests = None
    if integration_suite_name != "":
        integration_tests = runner.command(
            name=f"run {component_name} smoke tests",
            args=[
                MONGO_PYTHON_INTERPRETER,
                ROOT.joinpath("buildscripts", "run_smoke_tests.py"),
                "--suites",
                integration_suite_name,
            ],
            log_file="smoke_tests.log",
            deps=install,
        )

    unittests = None
    if unit_test_tag != "":
        unittests = runner.command(
            name=f"run {component_name} unittests",
            args=[
                BAZEL,
                "test",
                *bazel_args,
                f"--test_tag_filters={unit_test_tag}",
                "--test_output=summary",
                unit_test_build_target,
            ],
            # NOTE: bazel already stores the real logs somewhere else
            log_file="unittests.log",
            # not a true dep, but bazel access has to be serialized
            deps=install,
        )

    if run_clang_tidy:
        runner.command(
            name="clang tidy",
            args=[
                BAZEL,
                "build",
                # NOTE: don't use user-provided bazel args for clang-tidy
                "--config=clang-tidy",
                "--verbose_failures",
                "--keep_going",
                "//src/mongo/...",
            ],
            log_file="clang_tidy.log",
            # serializes bazel access. Also prevents clang-tidy from changing
            # the build config from under the smoke test suite before it's
            # finished running.
            deps=(integration_tests, unittests),
        )

    runner.run()


def main():
    from argparse import ArgumentParser

    p = ArgumentParser()

    p.add_argument(
        "component",
        type=str,
        help="Component that you wish to run the smoke test suite for. The available components are: catalog-and-routing, server-integration, replication, server-bsoncolumn, server-collection-write-path, server-external-sorter, server-index-builds, server-storage-engine-integration, server-timeseries-bucket-catalog, server-tracking-allocator, server-ttl",
    )

    p.add_argument(
        "--log-path",
        type=Path,
        help="Directory to place logs from smoke test stages",
        default=Path("~/.logs/smoke_tests").expanduser(),
    )
    p.add_argument(
        "--upstream-branch",
        type=str,
        default="origin/master",
        help="Git branch to format diff against",
    )

    p.add_argument(
        "--run-clang-tidy",
        type=bool,
        default=False,
        help="Run clang tidy. This might take a while to run, and will also change the build config to config=clang-tidy.",
    )

    p.add_argument(
        "--send-slack-notification",
        type=int,
        default=1,
        help='Send a slack notification based on the local evergreen configuration to "yourself"',
    )

    args, bazel_args = p.parse_known_args()
    run_smoke_tests(
        component_name=args.component,
        log_path=args.log_path,
        upstream_branch=args.upstream_branch,
        bazel_args=bazel_args,
        run_clang_tidy=args.run_clang_tidy,
        send_slack_notification=args.send_slack_notification,
    )


if __name__ == "__main__":
    main()
