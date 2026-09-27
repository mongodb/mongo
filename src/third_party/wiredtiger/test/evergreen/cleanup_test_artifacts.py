#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

"""Delete the test databases left behind by tests that passed.

Every CTest test runs in its own working directory and leaves its WiredTiger database
there when it finishes. Transaction logs are preallocated, so a full 'make check' run
leaves several gigabytes that are then packaged into the CI artifacts. The database is
only useful for a test that failed, so remove it for the tests that passed and keep it
for the ones that failed. Directories that other test commands share with CTest are left
alone, as a failure of one of those is not recorded in the CTest results.

The script is deliberately conservative: if it cannot tell which working directory a
failed test used, it removes nothing.
"""

import argparse
import fnmatch
import os
import re
import shutil
import stat

# Names a test uses for the WiredTiger database it creates in its working directory.
DATA_ENTRY_PATTERNS = ("WT_TEST*", "WT_HOME*", "WT_RD*", "WT_BLOCK*", "RUNDIR*")

# The generated CTest file records each test's name and, when set, its working directory.
ADD_TEST_RE = re.compile(r"add_test\(\[=\[(?P<name>.*?)\]=\]")
ADD_TEST_NAME_RE = re.compile(r"add_test\(NAME\s+(?P<name>\S+)")
SET_PROPERTIES_RE = re.compile(r"set_tests_properties\(\[=\[(?P<name>.*?)\]=\]")
WORKING_DIRECTORY_RE = re.compile(r'WORKING_DIRECTORY\s+"(?P<dir>[^"]*)"')

CTEST_TESTFILE = "CTestTestfile.cmake"
LAST_TEST_LOG = os.path.join("Testing", "Temporary", "LastTest.log")
LAST_FAILED_LOG = os.path.join("Testing", "Temporary", "LastTestsFailed.log")
BUILD_DIRECTORY_HEADER = "# Build directory:"


def find_ctest_dirs(build_dir):
    """Return the directories that contain CTest metadata, i.e. where a ctest run happened."""
    ctest_dirs = []
    for root, _, files in os.walk(build_dir):
        if CTEST_TESTFILE in files:
            ctest_dirs.append(root)
    return ctest_dirs


def parse_testfile(path):
    """Map each test defined in a CTestTestfile to its working directory.

    Working directories are recorded as absolute paths from the machine that configured the
    build. Rebase them onto this checkout so the test files can be read anywhere.
    """
    tests = {}
    base_dir = os.path.dirname(path)
    recorded_build_dir = None
    with open(path) as f:
        for line in f:
            if line.startswith(BUILD_DIRECTORY_HEADER):
                recorded_build_dir = line.split(":", 1)[1].strip()
                continue
            match = ADD_TEST_RE.search(line) or ADD_TEST_NAME_RE.search(line)
            if match:
                tests[match.group("name")] = base_dir
                continue
            match = SET_PROPERTIES_RE.search(line)
            if match:
                working_dir = WORKING_DIRECTORY_RE.search(line)
                if working_dir:
                    tests[match.group("name")] = rebase(
                        working_dir.group("dir"), base_dir, recorded_build_dir
                    )
    return tests


def rebase(working_dir, base_dir, recorded_build_dir):
    """Translate a recorded build path into a path in this build directory.

    The paths in the generated files are the ones from the machine that configured the build, so
    strip that build directory and re-root onto ours. This is done with plain string handling
    because the recorded path may use a different flavour (a Windows drive letter) than the
    platform running the script.
    """
    if recorded_build_dir:
        recorded = recorded_build_dir.replace("\\", "/").rstrip("/")
        path = working_dir.replace("\\", "/")
        if path == recorded:
            return base_dir
        if path.startswith(recorded + "/"):
            return os.path.join(base_dir, *path[len(recorded) + 1 :].split("/"))
    return working_dir


def make_writable(path):
    """Give the owner write and search permission on a path and everything below it."""
    os.chmod(path, stat.S_IRWXU)
    if os.path.isdir(path):
        for name in os.listdir(path):
            make_writable(os.path.join(path, name))


def remove_entry(path):
    """Remove a test data entry, which is either a database directory or a leftover file.

    Tests leave files as well as directories (for example the profiling data of a fuzz run), and
    some set read-only permissions on their data, so clear those and retry if the first attempt
    fails.
    """
    is_directory = os.path.isdir(path) and not os.path.islink(path)
    if is_directory:
        try:
            shutil.rmtree(path)
        except OSError:
            make_writable(path)
            shutil.rmtree(path)
    else:
        try:
            os.remove(path)
        except OSError:
            os.chmod(path, stat.S_IRWXU)
            os.remove(path)


def read_failed_tests(path):
    """Read the test names from a LastTestsFailed.log file (lines of the form '<index>:<name>')."""
    with open(path) as f:
        names = []
        for line in f:
            line = line.strip()
            if not line:
                continue
            index, separator, name = line.partition(":")
            names.append(name if separator and index.isdigit() else line)
        return names


def collect_tests(ctest_dirs):
    """Map each test to its working directory, and group tests by working directory."""
    tests = {}
    for ctest_dir in ctest_dirs:
        tests.update(parse_testfile(os.path.join(ctest_dir, CTEST_TESTFILE)))

    by_working_dir = {}
    for name, working_dir in tests.items():
        by_working_dir.setdefault(os.path.abspath(working_dir), []).append(name)
    return tests, by_working_dir


def is_ctest_only(working_dir):
    """Whether a working directory belongs to CTest alone.

    Some Evergreen tasks run test binaries directly in the same directories as CTest, for example
    the format, many dbs and thread tests that run after 'make check all'. Those commands write
    there too, and a failure of one of them is not recorded in the CTest results, so only the
    directories that exist purely for a CTest test - the per-variant test directories and the
    example directories - can be cleaned safely.
    """
    name = os.path.basename(working_dir.rstrip("/\\"))
    return name.endswith("_test_dir") or "examples" in working_dir.replace(
        "\\", "/"
    ).split("/")


def data_entries(working_dir):
    """Return the test database entries left in a working directory."""
    try:
        entries = os.listdir(working_dir)
    except OSError:
        return []
    return [
        os.path.join(working_dir, entry)
        for entry in entries
        if any(fnmatch.fnmatch(entry, pattern) for pattern in DATA_ENTRY_PATTERNS)
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-b",
        "--build-dir",
        default=".",
        help="The CMake build directory to clean up (defaults to the current directory).",
    )
    parser.add_argument(
        "-n",
        "--dry-run",
        action="store_true",
        help="Report what would be removed without removing anything.",
    )
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    ctest_dirs = find_ctest_dirs(build_dir)

    # Only clean up if a ctest run happened somewhere under the build directory. LastTest.log is
    # written for every run, including a run where all the tests passed.
    if not any(os.path.exists(os.path.join(d, LAST_TEST_LOG)) for d in ctest_dirs):
        print(f"No CTest results under {build_dir}, keeping all test data.")
        return 0

    tests, by_working_dir = collect_tests(ctest_dirs)

    failed = set()
    for ctest_dir in ctest_dirs:
        failed_log = os.path.join(ctest_dir, LAST_FAILED_LOG)
        if os.path.exists(failed_log):
            failed.update(read_failed_tests(failed_log))

    # If a failed test is not one we know about we cannot tell which directory to keep, so leave
    # everything in place rather than risk deleting the data of a failure.
    unknown = sorted(name for name in failed if name not in tests)
    if unknown:
        print(f"Unknown failed tests {unknown}, keeping all test data.")
        return 0

    if failed:
        print(f"Keeping test data for the failed tests: {', '.join(sorted(failed))}")

    removed = 0
    for working_dir, names in sorted(by_working_dir.items()):
        # Keep the data of a failed test, and leave the directories that other test commands
        # share with CTest alone.
        if any(name in failed for name in names) or not is_ctest_only(working_dir):
            continue
        for entry in data_entries(working_dir):
            print(f"{'Would remove' if args.dry_run else 'Removing'} {entry}")
            if not args.dry_run:
                remove_entry(entry)
            removed += 1

    print(
        f"{'Would remove' if args.dry_run else 'Removed'} {removed} test data directories."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
