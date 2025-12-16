import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import List

import typer


def process_bep(bep_path):
    failed_tests = []
    successful_tests = []
    with open(bep_path, "rt") as f:
        # Each line in the BEP JSON file is a separate JSON object representing an event
        for line in f:
            event = json.loads(line)
            if "testSummary" in event.get("id", {}):
                target_label = event["id"]["testSummary"]["label"]
                if "testSummary" not in event:
                    continue
                overall_status = event["testSummary"]["overallStatus"]
                if overall_status != "PASSED":
                    failed_tests += [target_label]
                else:
                    successful_tests += [target_label]
    return failed_tests, successful_tests


def _relink_binaries_with_symbols(failed_test_labels: List[str]):
    print("Rebuilding tests with --remote_download_outputs=toplevel...")
    bazel_build_flags = ""
    if os.path.isfile(".bazel_build_flags"):
        with open(".bazel_build_flags", "r", encoding="utf-8") as f:
            bazel_build_flags = f.read().strip()

    bazel_build_flags += " --remote_download_outputs=toplevel"

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


def _copy_bins_to_upload(upload_bin_dir: str, upload_lib_dir: str):
    libs = []
    bins = []
    dsyms = []
    bazel_bin_dir = Path("./bazel-bin/src")
    for dirpath, _, filenames in os.walk(bazel_bin_dir):
        if dirpath.endswith(".dSYM"):
            dsyms.append(Path(dirpath))
        for f in filenames:
            file = Path(f)
            if file.stem.endswith(("_with_debug", "_ci_wrapper")):
                continue
            if file.suffix in [".so", ".so.debug", ".dylib"]:
                libs.append(Path(os.path.join(dirpath, file)))
            elif file.suffix in [".debug", ".dwp", ".pdb", ".exe", ""]:
                bins.append(Path(os.path.join(dirpath, file)))

    for binary_file in bins:
        new_binary_file = upload_bin_dir / binary_file.name
        if not os.path.exists(new_binary_file):
            try:
                shutil.copy(binary_file, new_binary_file)
            except FileNotFoundError:
                continue  # It is likely a broken symlink.

    for lib_file in libs:
        new_lib_file = upload_lib_dir / lib_file.name
        if not os.path.exists(new_lib_file):
            try:
                shutil.copy(lib_file, new_lib_file)
            except FileNotFoundError:
                continue  # It is likely a broken symlink.

    for dsym_dir in dsyms:
        print(f"Copying dsym {dsym_dir} to {upload_bin_dir}")
        try:
            shutil.copytree(dsym_dir, upload_bin_dir / dsym_dir.name, dirs_exist_ok=True)
        except FileNotFoundError:
            continue  # It is likely a broken symlink.


def main(build_events: str = "build_events.json"):
    """Gather binaries and debug symbols of failed tests based off of a Build Event Protocol (BEP) json file."""

    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    upload_bin_dir = Path("dist-tests/bin")
    upload_lib_dir = Path("dist-tests/lib")
    upload_bin_dir.mkdir(parents=True, exist_ok=True)
    upload_lib_dir.mkdir(parents=True, exist_ok=True)

    failed_tests, successful_tests = process_bep(build_events)
    if len(failed_tests) == 0 and len(successful_tests) == 0:
        print("Test results not found, aborting. Please check above for any build errors.")
        exit(1)

    if not failed_tests:
        print("No failed tests found.")
        exit(0)

    print(f"Found {len(failed_tests)} failed tests. Gathering binaries and debug symbols.")
    _relink_binaries_with_symbols(failed_tests)

    print("Copying binaries and debug symbols to upload directories.")
    _copy_bins_to_upload(upload_bin_dir, upload_lib_dir)


if __name__ == "__main__":
    typer.run(main)
