import os
import pathlib
import sys
from functools import cache

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))
from buildscripts.resmokelib import cli


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

    if os.environ.get("TEST_SRCDIR"):
        test_srcdir = os.environ.get("TEST_SRCDIR")
        version_file = os.path.join(
            test_srcdir, "_main", "bazel", "resmoke", ".resmoke_mongo_version.yml"
        )
        link_path = os.path.join(test_srcdir, "_main", ".resmoke_mongo_version.yml")
        if not os.path.exists(link_path):
            os.symlink(
                version_file,
                link_path,
            )

    cli.main(resmoke_args)
