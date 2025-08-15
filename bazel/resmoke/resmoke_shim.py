import os
import pathlib
import signal
import sys
from functools import cache

import psutil

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))
from buildscripts.bazel_local_resources import acquire_local_resource
from buildscripts.resmokelib import cli
from buildscripts.resmokelib.hang_analyzer.process import signal_python
from buildscripts.resmokelib.logging.loggers import new_resmoke_logger


@cache
def get_volatile_status() -> dict:
    volatile_status = {}
    with open("bazel/resmoke/volatile-status.txt", "rt") as f:
        for line in f:
            key, val = line.strip().split(" ", 1)[:2]
            volatile_status[key] = val
    return volatile_status


def get_from_volatile_status(key):
    volatile_status = get_volatile_status()
    return volatile_status.get(key)


def add_volatile_arg(args, flag, key):
    val = get_from_volatile_status(key)
    if val:
        args.append(flag + val)


def add_evergreen_build_info(args):
    add_volatile_arg(args, "--buildId=", "build_id")
    add_volatile_arg(args, "--distroId=", "distro_id")
    add_volatile_arg(args, "--executionNumber=", "execution")
    add_volatile_arg(args, "--projectName=", "project")
    add_volatile_arg(args, "--gitRevision=", "revision")
    add_volatile_arg(args, "--revisionOrderId=", "revision_order_id")
    add_volatile_arg(args, "--taskId=", "task_id")
    add_volatile_arg(args, "--taskName=", "task_name")
    add_volatile_arg(args, "--variantName=", "build_variant")
    add_volatile_arg(args, "--versionId=", "version_id")
    add_volatile_arg(args, "--requester=", "requester")

class ResmokeShimContext:
    def __init__(self):
        self.links = []
        
    def __enter__(self):
        # Bazel will send SIGTERM on a test timeout. If all processes haven't terminated
        # after –-local_termination_grace_seconds (default 15s), Bazel will SIGKILL them instead.
        signal.signal(signal.SIGTERM, self._handle_interrupt)

        # Symlink source directories because resmoke uses relative paths profusely.
        base_dir = os.path.join(os.environ.get("TEST_SRCDIR"), "_main")
        working_dir = os.getcwd()
        for entry in os.scandir(base_dir):
            link = os.path.join(working_dir, entry.name)
            self.links.append(link)
            os.symlink(entry.path, link)
        return self

    def __exit__(self, exception_type, exception_value, exception_traceback):
        for link in self.links:
            os.unlink(link)

    def _handle_interrupt(self, signum, frame):
        # Attempt a clean shutdown, producing python stacktraces and generating core dumps for
        # any still running process. It is likely that most programs will have terminated before
        # core dumps can be produced, since Bazel sends SIGTERM to all processes, not just this one.
        # TODO: SERVER-109274
        pid = os.getpid()
        p = psutil.Process(pid)
        signal_python(new_resmoke_logger(), p.name, pid)

if __name__ == "__main__":
    sys.argv[0] = (
        "buildscripts/resmoke.py"  # Ensure resmoke's local invocation is printed using resmoke.py directly
    )
    resmoke_args = sys.argv

    # If there was an extra --suites argument added as a --test_arg in the bazel invocation, rewrite
    # the original as --originSuite. Intentionally 'suite', sinces it is a common partial match for the actual
    # argument 'suites'
    suite_args = [i for i, arg in enumerate(resmoke_args) if arg.startswith("--suite")]
    if len(suite_args) > 1:
        resmoke_args[suite_args[0]] = resmoke_args[suite_args[0]].replace(
            "--suites", "--originSuite"
        )

    add_evergreen_build_info(resmoke_args)

    if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
        undeclared_output_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
        resmoke_args.append(f"--dbpathPrefix={os.path.join(undeclared_output_dir,'data')}")
        resmoke_args.append(f"--taskWorkDir={undeclared_output_dir}")
        resmoke_args.append(f"--reportFile={os.path.join(undeclared_output_dir,'report.json')}")
        os.chdir(undeclared_output_dir)

    if os.environ.get("TEST_SHARD_INDEX") and os.environ.get("TEST_TOTAL_SHARDS"):
        shard_count = os.environ.get("TEST_TOTAL_SHARDS")
        shard_index = os.environ.get("TEST_SHARD_INDEX")

        resmoke_args.append(f"--shardIndex={shard_index}")
        resmoke_args.append(f"--shardCount={shard_count}")
        if os.environ.get("TEST_SHARD_STATUS_FILE"):
            open(os.environ["TEST_SHARD_STATUS_FILE"], "w").close()

        report = f"report_shard_{shard_index}_of_{shard_count}.json"
    else:
        report = "report.json"
    resmoke_args.append(f"--reportFile={os.path.join(undeclared_output_dir, report)}")

    lock, base_port = acquire_local_resource("port_block")
    resmoke_args.append(f"--basePort={base_port}")

    if (
        os.path.isfile("bazel/resmoke/test_runtimes.json")
        and os.path.getsize("bazel/resmoke/test_runtimes.json") != 0
    ):
        resmoke_args.append("--historicTestRuntimes=bazel/resmoke/test_runtimes.json")

    with ResmokeShimContext() as ctx:
        cli.main(resmoke_args)

    lock.release()
