import json
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path
from zipfile import BadZipFile, ZipFile

import typer


def _bazel_binary() -> str:
    return os.environ.get("BAZEL_BINARY", "bazel")


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


def _symbol_download_flags(build_flags: list[str]) -> list[str]:
    """Materialize debug files while retaining the saved build configuration and download regex."""
    regex = None
    for index, flag in enumerate(build_flags):
        if flag.startswith("--remote_download_regex="):
            regex = flag.split("=", 1)[1]
        elif flag == "--remote_download_regex" and index + 1 < len(build_flags):
            regex = build_flags[index + 1]
    symbols = r".*\.(pdb|debug|dwp)$"
    combined = f"(?:{regex})|(?:{symbols})" if regex else symbols
    return [
        *build_flags,
        "--remote_download_outputs=toplevel",
        f"--remote_download_regex={combined}",
    ]


def _relink_binaries_with_symbols(failed_test_labels: list[str]) -> list[str]:
    print("Rebuilding tests with --remote_download_outputs=toplevel...")
    bazel_build_flags = []
    if os.path.isfile(".bazel_build_flags"):
        with open(".bazel_build_flags", "r", encoding="utf-8") as f:
            bazel_build_flags = shlex.split(f.read())

    bazel_build_flags = _symbol_download_flags(bazel_build_flags)

    relink_command = [_bazel_binary(), "build", *bazel_build_flags, *failed_test_labels]

    print(f"Running command: {' '.join(relink_command)}")
    subprocess.run(
        relink_command,
        check=True,
    )

    repro_test_command = shlex.join(
        [_bazel_binary(), "test", *bazel_build_flags, *failed_test_labels]
    )
    with open(".failed_unittest_repro.txt", "w", encoding="utf-8") as f:
        f.write(repro_test_command)
    print(f"Repro command written to .failed_unittest_repro.txt: {repro_test_command}")

    return bazel_build_flags


def _testlogs_dir_for_label(label: str) -> Path:
    """Maps a test label (//src/mongo/logv2:logv2_test) to its bazel-testlogs directory."""
    return Path("bazel-testlogs", *label.removeprefix("//").replace(":", "/").split("/"))


def _find_testlogs_dirs(failed_test_labels: list[str]) -> list[Path]:
    """Finds testlogs in the default and transitioned configurations, without symlink aliases."""
    testlogs_dirs = set()
    for label in failed_test_labels:
        default_dir = _testlogs_dir_for_label(label)
        target_path = default_dir.relative_to("bazel-testlogs")
        candidates = [
            default_dir,
            *sorted(Path("bazel-out").glob(f"*/testlogs/{target_path.as_posix()}")),
        ]
        # Resolve before invoking Bazel: its convenience symlink may point to a different
        # configuration after relinking or rerunning. Retain even a missing default path
        # so cleanup can remove results created for a test that originally failed to build.
        testlogs_dirs.update(path.resolve() for path in candidates)
    return sorted(testlogs_dirs)


def _snapshot_test_logs(testlogs_dirs: list[Path]) -> dict[Path, bytes]:
    """Preserves the original test.xml/test.log entries of the failed tests.

    The diagnostic rerun re-executes the tests and overwrites their bazel-testlogs entries;
    the create_bazel_test_report teardown step must still report the results of the original
    run, otherwise a flaky test that passes on rerun would show up as passing on a failed task.
    """
    snapshot = {}
    for testlogs_dir in testlogs_dirs:
        for log_name in ["test.xml", "test.log"]:
            # Sharding, repeated runs, and flaky-test attempts put their logs in
            # nested directories, which the Evergreen report also reads recursively.
            for log_path in testlogs_dir.rglob(log_name):
                if log_path.is_file():
                    snapshot[log_path] = log_path.read_bytes()
    return snapshot


def _restore_testlogs_link(original_target: Path | None) -> None:
    """Keeps the original logs reachable by Evergreen's bazel-testlogs report reader."""
    if original_target is None:
        return
    link = Path("bazel-testlogs")
    try:
        if link.is_symlink() or link.is_junction():
            if link.readlink() == original_target:
                return
            if link.is_junction():
                link.rmdir()
            else:
                link.unlink()
        link.symlink_to(original_target, target_is_directory=True)
    except OSError as e:
        print(f"Failed to restore {link} to {original_target}: {e}")


def _restore_test_logs(
    snapshot: dict[Path, bytes], testlogs_dirs: list[Path] | None = None
) -> None:
    # A rerun can create additional run/attempt XML files. Remove only its new
    # result logs so the report sees the original result set; keep diagnostic
    # artifacts and other targets' logs for upload.
    for testlogs_dir in testlogs_dirs or []:
        for log_name in ["test.xml", "test.log"]:
            for log_path in testlogs_dir.rglob(log_name):
                if log_path in snapshot or not log_path.is_file():
                    continue
                try:
                    os.chmod(log_path, 0o644)
                    log_path.unlink()
                except OSError as e:
                    print(f"Failed to remove diagnostic rerun log {log_path}: {e}")

    for log_path, contents in snapshot.items():
        try:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            # Bazel writes the testlogs entries read-only (on Windows the read-only
            # attribute makes write_bytes fail with PermissionError).
            if log_path.exists():
                os.chmod(log_path, 0o644)
            log_path.write_bytes(contents)
        except OSError as e:
            # Best-effort: keep gathering the binaries even if a log cannot be restored.
            print(f"Failed to restore {log_path}: {e}")


def _archive_test_outputs(testlogs_dirs: list[Path]) -> None:
    """Keeps original undeclared outputs outside the tree Bazel clears on a diagnostic rerun."""
    archive_dir = None
    output_index = 0
    for testlogs_dir in testlogs_dirs:
        sources = [path for path in testlogs_dir.rglob("test.outputs") if path.is_dir()]
        # Bazel versions/layouts can place outputs.zip directly in the shard directory.
        sources.extend(
            path
            for path in testlogs_dir.rglob("outputs.zip")
            if "test.outputs" not in path.relative_to(testlogs_dir).parts[:-1]
        )
        for source in sorted(sources):
            if archive_dir is None:
                Path("failed-test-outputs").mkdir(exist_ok=True)
                archive_dir = Path(tempfile.mkdtemp(prefix="original-", dir="failed-test-outputs"))
            output_archive = archive_dir / str(output_index) / "test.outputs"
            if source.is_dir():
                shutil.copytree(source, output_archive)
            else:
                output_archive.mkdir(parents=True)
                shutil.copy2(source, output_archive / "outputs.zip")
            zip_path = output_archive / "outputs.zip"
            if zip_path.is_file():
                # Extract into the preserved tree so the existing core collector can
                # discover dumps and boring_core_dumps.txt without inspecting zip files.
                # Copied Bazel outputs may be read-only, including any unzipped entries.
                output_archive.chmod(0o755)
                for path in output_archive.rglob("*"):
                    path.chmod(0o755 if path.is_dir() else 0o644)
                with ZipFile(zip_path) as archive:
                    archive.extractall(output_archive)
            # Evergreen flattens dump paths during collection. Keep every original dump
            # distinct from other shards and the diagnostic rerun, while preserving the
            # extension and trailing PID used by the core analyzer.
            for index, dump in enumerate(sorted(output_archive.rglob("*"))):
                if dump.is_file() and (
                    dump.suffix in [".core", ".mdmp"] or dump.name.endswith(".core.gz")
                ):
                    dump.chmod(0o644)
                    dump.rename(
                        dump.with_name(f"{archive_dir.name}-{output_index}-{index}-{dump.name}")
                    )
            output_index += 1


def _find_bazel_bin_dirs() -> list[Path]:
    candidates = [Path("bazel-bin/src"), *Path("bazel-out").glob("*/bin/src")]
    return sorted({path.resolve() for path in candidates if path.is_dir()})


def _archive_crash_handler_dumps() -> None:
    """Preserves minidumps written beside binaries before Bazel changes their output trees."""
    dumps = sorted(
        {
            dump.resolve()
            for binary_dir in _find_bazel_bin_dirs()
            for dump in binary_dir.rglob("*.mdmp")
            if dump.is_file()
        }
    )
    if not dumps:
        return
    Path("failed-test-outputs").mkdir(exist_ok=True)
    archive_dir = Path(tempfile.mkdtemp(prefix="original-crash-", dir="failed-test-outputs"))
    for index, dump in enumerate(dumps):
        # The core collector flattens paths. Distinguish configurations and reruns
        # while retaining the original executable name, timestamp/PID, and extension.
        shutil.copy2(dump, archive_dir / f"{archive_dir.name}-{index}-{dump.name}")


def _rerun_failed_tests(failed_test_labels: list[str], bazel_build_flags: list[str]) -> None:
    """Reruns failed tests with the original build configuration and downloaded symbols.

    Failures are expected; this diagnostic run captures crash artifacts while the
    original logs remain the source of the task result.
    """
    # Diagnostic reruns must execute even when the original flags enable cached results.
    rerun_command = [
        _bazel_binary(),
        "test",
        *bazel_build_flags,
        "--nocache_test_results",
        # The core collector reads dump files directly rather than opening outputs.zip.
        "--nozip_undeclared_test_outputs",
        "--test_strategy=standalone",
        "--strategy=TestRunner=standalone",
        *failed_test_labels,
    ]

    print(f"Running command: {' '.join(rerun_command)}")
    result = subprocess.run(
        rerun_command,
        check=False,
    )
    if result.returncode != 0:
        print(f"Rerun of failed tests exited with code {result.returncode}; continuing.")


def _copy_bins_to_upload(upload_bin_dir: str, upload_lib_dir: str):
    libs = []
    bins = []
    dsyms = []
    for bazel_bin_dir in _find_bazel_bin_dirs():
        for dirpath, _, filenames in os.walk(bazel_bin_dir):
            if dirpath.endswith(".dSYM"):
                dsyms.append(Path(dirpath))
            for f in filenames:
                file = Path(f)
                if file.stem.endswith("_with_debug"):
                    continue
                if file.suffix in [".so", ".so.debug", ".dylib"]:
                    libs.append(Path(os.path.join(dirpath, file)))
                # .mdmp minidumps (Windows) are written next to the test binary inside its
                # runfiles tree on crash; they pair with the .pdb for post-mortem debugging.
                elif file.suffix in [".debug", ".dwp", ".pdb", ".exe", ".mdmp", ""]:
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
    """Gathers failed-test binaries and symbols, then reruns tests locally for crash artifacts."""

    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    build_events_path = Path(build_events)
    if not build_events_path.is_file():
        print(
            f"Build events file {build_events_path} was not found; skipping failed test "
            "binary gathering. This can happen when Bazel fails before the test phase emits BEP."
        )
        return

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
    try:
        # Windows crash-handler dumps are beside executables, outside test.outputs.
        # Preserve them before relinking can replace binaries or their runfiles trees.
        _archive_crash_handler_dumps()
    except (OSError, shutil.Error) as e:
        print(f"Failed to preserve crash-handler dumps; skipping relink and diagnostic rerun: {e}")
        _copy_bins_to_upload(upload_bin_dir, upload_lib_dir)
        return
    # The rerun re-executes the failed tests and overwrites their bazel-testlogs entries, so
    # preserve the original logs and undeclared outputs before invoking Bazel again.
    original_testlogs_dirs = _find_testlogs_dirs(failed_tests)
    testlogs_link = Path("bazel-testlogs")
    original_link_target = (
        testlogs_link.readlink()
        if testlogs_link.is_symlink() or testlogs_link.is_junction()
        else None
    )
    original_test_logs = {}
    can_rerun = False
    try:
        original_test_logs = _snapshot_test_logs(original_testlogs_dirs)
        _archive_test_outputs(original_testlogs_dirs)
        can_rerun = True
    except (OSError, shutil.Error, BadZipFile) as e:
        print(f"Failed to preserve original test outputs; skipping diagnostic rerun: {e}")

    try:
        bazel_build_flags = _relink_binaries_with_symbols(failed_tests)
        if can_rerun:
            _rerun_failed_tests(failed_tests, bazel_build_flags)
    finally:
        try:
            if can_rerun:
                # Include newly created configurations as well as the retained original paths.
                testlogs_dirs = sorted(
                    set(original_testlogs_dirs) | set(_find_testlogs_dirs(failed_tests))
                )
                _restore_test_logs(original_test_logs, testlogs_dirs)
        finally:
            _restore_testlogs_link(original_link_target)
            print("Copying binaries and debug symbols to upload directories.")
            _copy_bins_to_upload(upload_bin_dir, upload_lib_dir)


app = typer.Typer(pretty_exceptions_show_locals=False)
app.command()(main)

if __name__ == "__main__":
    app()
