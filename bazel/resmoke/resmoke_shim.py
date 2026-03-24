import os
import pathlib
import signal
import sys
import tempfile
import uuid
from functools import cache

import psutil

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))
from bazel.resmoke.resource_monitor import ResourceMonitor
from buildscripts.bazel_local_resources import acquire_local_resource
from buildscripts.resmokelib import cli
from buildscripts.resmokelib.hang_analyzer.process import signal_python
from buildscripts.resmokelib.logging.loggers import new_resmoke_logger


@cache
def get_volatile_status() -> dict:
    volatile_status = {}
    with open(os.path.join("bazel", "resmoke", "volatile-status.txt"), "rt") as f:
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
    add_volatile_arg(args, "--gitRevision=", "revision")
    add_volatile_arg(args, "--otelParentId=", "otel_parent_id")
    add_volatile_arg(args, "--otelTraceId=", "otel_trace_id")
    add_volatile_arg(args, "--projectName=", "project")
    add_volatile_arg(args, "--requester=", "requester")
    add_volatile_arg(args, "--revisionOrderId=", "revision_order_id")
    add_volatile_arg(args, "--taskId=", "task_id")
    add_volatile_arg(args, "--taskName=", "task_name")
    add_volatile_arg(args, "--variantName=", "build_variant")
    add_volatile_arg(args, "--versionId=", "version_id")


def setup_pythonpath():
    """Setup PYTHONPATH and executable location for jstests that call python"""

    os.environ["RESMOKE_PYTHON"] = sys.executable

    python_imports_file = os.environ.get("PYTHON_IMPORTS_FILE")
    if not python_imports_file or not os.path.exists(python_imports_file):
        return

    with open(python_imports_file, "r") as f:
        imports = [line.strip() for line in f if line.strip()]

    if not imports:
        return

    # Convert runfiles-relative paths to absolute paths
    test_srcdir = os.environ.get("TEST_SRCDIR")
    if not test_srcdir:
        return
    import_paths = [os.path.join(test_srcdir, imp) for imp in imports]

    existing_pythonpath = os.environ.get("PYTHONPATH", "")
    new_pythonpath = os.pathsep.join(import_paths)
    if existing_pythonpath:
        new_pythonpath = new_pythonpath + os.pathsep + existing_pythonpath

    os.environ["PYTHONPATH"] = new_pythonpath


class ResmokeShimContext:
    def __init__(self):
        self.links = []
        self.tmpdir_symlink = None
        self.outputs_symlink = None
        self.resource_monitor = None

    def create_short_symlinks(self):
        """Create short symlinks in the original tmpdir to avoid long path issues."""
        original_tmpdir = tempfile.gettempdir()

        # Create a short symlink to TEST_TMPDIR
        test_tempdir = os.environ.get("TEST_TMPDIR")
        if test_tempdir:
            self.tmpdir_symlink = os.path.join(original_tmpdir, f"resmoke_tmp_{uuid.uuid1()}")
            os.symlink(test_tempdir, self.tmpdir_symlink)
            self.links.append(self.tmpdir_symlink)

        # Create a short symlink to TEST_UNDECLARED_OUTPUTS_DIR
        undeclared_outputs_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
        if undeclared_outputs_dir:
            self.outputs_symlink = os.path.join(original_tmpdir, f"resmoke_out_{uuid.uuid1()}")
            os.symlink(undeclared_outputs_dir, self.outputs_symlink)
            self.links.append(self.outputs_symlink)

    def __enter__(self):
        # Use the Bazel provided TEST_TMPDIR. Note this must occur after uses of acquire_local_resource
        # which relies on a shared temporary directory among all test shards.
        if self.tmpdir_symlink:
            os.environ["TMPDIR"] = self.tmpdir_symlink
            os.environ["TMP"] = self.tmpdir_symlink
            os.environ["TEMP"] = self.tmpdir_symlink

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

        try:
            output_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
            if output_dir:
                output_file = os.path.join(output_dir, "resource_usage.txt")
                self.resource_monitor = ResourceMonitor(
                    output_file=output_file, root_pid=os.getpid()
                )
                self.resource_monitor.write_snapshot(self.resource_monitor.collect_snapshot())
                self.resource_monitor.start_periodic_monitoring()
        except Exception as e:
            # Log but don't fail - monitoring is non-critical
            print(f"Warning: Failed to initialize resource monitoring: {e}", file=sys.stderr)
            self.resource_monitor = None

        return self

    def __exit__(self, exception_type, exception_value, exception_traceback):
        if self.resource_monitor:
            try:
                self.resource_monitor.stop_periodic_monitoring()
                final_snapshot = self.resource_monitor.collect_snapshot()
                self.resource_monitor.write_snapshot(final_snapshot)
            except Exception as e:
                print(f"Warning: Error during resource monitoring cleanup: {e}", file=sys.stderr)

        for link in self.links:
            os.unlink(link)

    def _handle_interrupt(self, signum, frame):
        # Attempt a clean shutdown, producing python stacktraces and generating core dumps for
        # any still running process. It is likely that most programs will have terminated before
        # core dumps can be produced, since Bazel sends SIGTERM to all processes, not just this one.
        # Individual timeouts per test are depended upon for useful core dumps of test processes.
        pid = os.getpid()
        p = psutil.Process(pid)
        signal_python(new_resmoke_logger(), p.name, pid)


if __name__ == "__main__":
    setup_pythonpath()

    sys.argv[0] = os.path.join(
        "buildscripts", "resmoke.py"
    )  # Ensure resmoke's local invocation is printed using resmoke.py directly
    resmoke_args = sys.argv

    # If there was an extra --suites argument added as a --test_arg in the bazel invocation, rewrite
    # the original as --originSuite. Intentionally 'suite', sinces it is a common partial match for the actual
    # argument 'suites'
    suite_args = [i for i, arg in enumerate(resmoke_args) if arg.startswith("--suite")]
    if len(suite_args) > 1:
        resmoke_args[suite_args[0]] = resmoke_args[suite_args[0]].replace(
            "--suites", "--originSuite"
        )

    # Reduce the storage engine's cache size (if not already set) to reduce the likelihood
    # of a mongod process being killed by the OOM killer. The configuration of the
    # cache size is duplicated in evergreen/resmoke_tests_execute.sh, please update both.
    storage_engine_in_memory = "--storageEngine=inMemory" in resmoke_args
    storage_engine_cache_arg = [
        arg for arg in resmoke_args if arg.startswith("--storageEngineCacheSizeGB")
    ]
    if not storage_engine_cache_arg:
        if storage_engine_in_memory:
            resmoke_args.append("--storageEngineCacheSizeGB=4")
        else:
            resmoke_args.append("--storageEngineCacheSizeGB=1")

    add_evergreen_build_info(resmoke_args)

    if os.environ.get("DEPS_PATH"):
        # Modify DEPS_PATH to use os.pathsep, rather than ':'
        os.environ["PATH"] += os.pathsep + os.pathsep.join(
            [
                os.path.dirname(os.path.abspath(path))
                for path in os.environ.get("DEPS_PATH").split(":")
            ]
        )

    ctx = ResmokeShimContext()
    ctx.create_short_symlinks()

    undeclared_output_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")

    outputs_dir = ctx.outputs_symlink if ctx.outputs_symlink else undeclared_output_dir

    resmoke_args.append(f"--taskWorkDir={outputs_dir}")
    resmoke_args.append(f"--reportFile={os.path.join(outputs_dir, 'report.json')}")
    os.chdir(outputs_dir)

    # Locally, it is nice for the data directory to preserved in the test output. However, we
    # don't want to save it in CI since we explicitly archive the data directory for failed
    # tests already. It will add to the output tree size in remote execution which is wasteful.
    if "--log=evg" in resmoke_args:
        dbpath = ctx.tmpdir_symlink if ctx.tmpdir_symlink else os.environ.get("TEST_TMPDIR")
    else:
        dbpath = os.path.join(outputs_dir, "data")
    resmoke_args.append(f"--dbpathPrefix={dbpath}")

    if os.environ.get("TEST_SHARD_INDEX") and os.environ.get("TEST_TOTAL_SHARDS"):
        shard_count = os.environ.get("TEST_TOTAL_SHARDS")
        shard_index = os.environ.get("TEST_SHARD_INDEX")

        resmoke_args.append(f"--shardIndex={shard_index}")
        resmoke_args.append(f"--shardCount={shard_count}")
        if os.environ.get("TEST_SHARD_STATUS_FILE"):
            open(os.environ["TEST_SHARD_STATUS_FILE"], "w").close()

        report = f"report_shard_{shard_index}_of_{shard_count}.json"
    else:
        resmoke_args.append("--shardIndex=0")
        resmoke_args.append("--shardCount=1")

        report = "report.json"
    resmoke_args.append(f"--reportFile={os.path.join(outputs_dir, report)}")

    lock, base_port = acquire_local_resource("port_block")
    resmoke_args.append(f"--basePort={base_port}")

    resmoke_args.append(f"--archiveDirectory={os.path.join(outputs_dir, 'data_archives')}")

    with ctx:
        cli.main(resmoke_args)

    lock.release()
