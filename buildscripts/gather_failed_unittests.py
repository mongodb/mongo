import os
import shutil
import subprocess
import xml.etree.ElementTree as ET
from glob import glob
from pathlib import Path
from typing import List

import typer


def _collect_failed_tests(testlog_dir: str) -> List[str]:
    failed_tests = []
    for test_xml in glob(f"{testlog_dir}/**/test.xml", recursive=True):
        testsuite = ET.parse(test_xml).getroot().find("testsuite")
        testcase = testsuite.find("testcase")
        test_file = testcase.attrib["name"]

        if testcase.find("error") is not None:
            failed_tests += [test_file]
    return failed_tests


def _relink_binaries_with_symbols(failed_tests: List[str]):
    print("Rebuilding unit tests with --remote_download_outputs=toplevel...")
    bazel_build_flags = ""
    if os.path.isfile(".bazel_build_flags"):
        with open(".bazel_build_flags", "r", encoding="utf-8") as f:
            bazel_build_flags = f.read().strip()

    bazel_build_flags += " --remote_download_outputs=toplevel"

    # Remap //src/mongo/testabc to //src/mongo:testabc
    failed_test_labels = [
        ":".join(test.rsplit("/", 1)) for test in failed_tests
    ]

    relink_command = [
        arg for arg in ["bazel", "build", *bazel_build_flags.split(" "), *failed_test_labels] if arg
    ]

    print(f"Running command: {' '.join(relink_command)}")
    subprocess.run(
        relink_command,
        check=True,
    )

    repro_test_command = " ".join(["test" if arg == "build" else arg for arg in relink_command])
    with open(".failed_unittest_repro.txt", "w", encoding="utf-8") as f:
        f.write(repro_test_command)
    print(f"Repro command written to .failed_unittest_repro.txt: {repro_test_command}")

def _copy_bins_to_upload(failed_tests: List[str], upload_bin_dir: str, upload_lib_dir: str) -> bool:
    success = True
    bazel_bin_dir = Path("./bazel-bin/src")
    for failed_test in failed_tests:
        full_binary_path = bazel_bin_dir / failed_test
        binary_name = failed_test.split(os.sep)[-1]
        bin_to_upload = []
        for pattern in [
            "*.core",
            "*.mdmp",
            f"{binary_name}.debug",
            f"{binary_name}.pdb",
            f"{binary_name}.exe",
            f"{binary_name}",
        ]:
            bin_to_upload.extend(bazel_bin_dir.rglob(pattern))

        # core dumps may be in the root directory
        bin_to_upload.extend(Path(".").rglob("*.core"))
        bin_to_upload.extend(Path(".").rglob("*.mdmp"))

        if not bin_to_upload:
            print(f"Cannot locate the files to upload for ({failed_test})")
            success = False
            continue

        for binary_file in bin_to_upload:
            new_binary_file = upload_bin_dir / binary_file.name
            if not os.path.exists(new_binary_file):
                print(f"Copying {binary_file} to {new_binary_file}")
                shutil.copy(binary_file, new_binary_file)

        dsym_dir = full_binary_path.with_suffix(".dSYM")
        if dsym_dir.is_dir():
            print(f"Copying dsym {dsym_dir} to {upload_bin_dir}")
            shutil.copytree(dsym_dir, upload_bin_dir / dsym_dir.name, dirs_exist_ok=True)

        # Copy debug symbols for dynamic builds
        lib_to_upload = []
        for pattern in [
            "*.so",
            "*.dylib",
        ]:
            lib_to_upload.extend(bazel_bin_dir.rglob(pattern))

        for lib_file in lib_to_upload:
            new_lib_file = upload_lib_dir / lib_file.name
            if not os.path.exists(new_lib_file):
                print(f"Copying {lib_file} to {new_lib_file}")
                shutil.copy(lib_file, new_lib_file)
    print("All binaries and debug symbols copied successfully.")
    return success


def main(testlog_dir: str = "bazel-testlogs"):
    """Gather unit test binaries and debug symbols of failed unit tests based off of bazel test logs."""

    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    upload_bin_dir = Path("dist-unittests/bin")
    upload_lib_dir = Path("dist-unittests/lib")
    upload_bin_dir.mkdir(parents=True, exist_ok=True)
    upload_lib_dir.mkdir(parents=True, exist_ok=True)

    failed_tests = _collect_failed_tests(testlog_dir)
    if not failed_tests:
        print("No failed tests found.")
        exit(0)

    print(f"Found {len(failed_tests)} failed tests. Gathering binaries and debug symbols.")
    _relink_binaries_with_symbols(failed_tests)

    print("Copying binaries and debug symbols to upload directories.")
    if not _copy_bins_to_upload(failed_tests, upload_bin_dir, upload_lib_dir):
        print("Fatal error occurred during processing.")
        # TODO: add slack notification
        exit(1)


if __name__ == "__main__":
    typer.run(main)
