import glob
import os
import platform
import subprocess
import sys

from buildscripts.util.expansions import get_expansion

# This script is used to gather code coverage data from the build.
# It is run as part of the Evergreen build process.
# It is not intended to be run directly.


def get_bazel_coverage_report_file() -> str:
    BAZEL_BINARY = "bazel"  # simplified since we already restrict which platforms can run coverage
    proc = subprocess.run([BAZEL_BINARY, "info", "output_path"], check=True, capture_output=True)
    bazel_output_location = proc.stdout.decode("utf-8").strip()
    bazel_coverage_report_location = os.path.join(
        bazel_output_location, "_coverage", "_coverage_report.dat"
    )
    return bazel_coverage_report_location


def main():
    should_gather_code_coverage = get_expansion("gather_code_coverage_results", False)
    if not should_gather_code_coverage:
        print("Missing 'gather_code_coverage_results' expansion, skipping code coverage.")
        return 0

    gcov_binary = get_expansion("gcov_tool", None)
    if not gcov_binary:
        print(
            "Missing 'gcov_tool' expansion, skipping code coverage because this is likely not a code coverage variant."
        )
        return 0

    disallowed_arches = {"s390x", "s390", "ppc64le", "ppc64", "ppc", "ppcle"}
    arch = platform.uname().machine.lower()
    print(f"Detected arch: {arch}")
    if arch in disallowed_arches:
        raise RuntimeError(f"Code coverage not supported on architecture '{arch}'.")

    if not os.path.exists(".git"):
        raise RuntimeError(
            "No git repo found in working directory. Code coverage needs git repo to function."
        )

    bazel_coverage_report_location = get_bazel_coverage_report_file()
    if os.path.exists(bazel_coverage_report_location):
        print("Found bazel coverage report.")
        # no gcda files are generated from bazel coverage so we can exit early here
        return 0

    print(f"No bazel coverage report found at {bazel_coverage_report_location}")

    # because of bazel symlink shenanigans, the bazel gcda and gcno files are put in different
    # directories when the GCOV_PREFIX and GCOV_PREFIX_STRIP env vars are used. We manually
    # put the gcno files where the gcda files are generated to fix this.
    has_bazel_gcno = False
    workdir = get_expansion("workdir")
    bazel_output_dir = os.path.join(workdir, "bazel-out")
    for file in glob.iglob("./**/bazel-out/**/*.gcno", root_dir=workdir, recursive=True):
        has_bazel_gcno = True
        parts = file.split("bazel-out/")
        assert len(parts) == 2, "Something went wrong, path was not split into 2 parts."
        old_path = os.path.join(workdir, file)
        new_path = os.path.join(bazel_output_dir, parts[1])
        new_dir = os.path.dirname(new_path)
        os.makedirs(new_dir, exist_ok=True)
        os.rename(old_path, new_path)

    if not has_bazel_gcno:
        raise RuntimeError("Neither bazel coverage nor gcno files were found.")

    coveralls_report = "gcovr-coveralls.json"

    args = [
        "python3",
        "-m",
        "gcovr",
        "--output",
        coveralls_report,
        "--coveralls-pretty",
        "--txt",
        "gcovr-coveralls.txt",
        "--print-summary",
        "--exclude",
        "build/debug/.*",
        "--exclude",
        ".*bazel-out/.*",
        "--exclude",
        ".*external/.*mongo_toolchain.*",
        "--exclude",
        r".*src/.*_gen\.(h|hpp|cpp)",
        "--exclude",
        r".*src/mongo/db/cst/grammar\.yy",
        "--exclude",
        ".*src/mongo/idl/.*",
        "--exclude",
        r".*src/mongo/.*_test\.(h|hpp|cpp)",
        "--exclude",
        ".*src/mongo/dbtests/.*",
        "--exclude",
        ".*src/mongo/unittest/.*",
        "--exclude",
        ".*/third_party/.*",
        "--gcov-ignore-errors",
        "source_not_found",
        "--gcov-ignore-parse-errors",
        "negative_hits.warn",
        "--gcov-exclude-directories",
        ".*src/mongo/dbtests/.*",
        "--gcov-exclude-directories",
        ".*src/mongo/idl/.*",
        "--gcov-exclude-directories",
        ".*src/mongo/unittest/.*",
        "--gcov-exclude-directories",
        ".*/third_party/.*",
        "--gcov-executable",
        gcov_binary,
        bazel_output_dir,
    ]

    print("Running gcovr command")
    process = subprocess.run(
        args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8"
    )
    print(process.stdout)
    if process.returncode != 0:
        raise RuntimeError(f"gcovr failed with code: {process.returncode}")

    if not os.path.exists(coveralls_report):
        raise RuntimeError(f"Could not find coveralls json report at {coveralls_report}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
