"""Extracts `mongo-debugsymbols.tgz` in an idempotent manner for performance."""

import concurrent.futures
import glob
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
from logging import Logger
from pathlib import Path
from typing import Callable, Optional

from opentelemetry import trace
from opentelemetry.trace.status import StatusCode
from retry import retry

from buildscripts.resmokelib.hang_analyzer.dumper import Dumper
from buildscripts.resmokelib.run.runtime_recorder import compare_start_time
from buildscripts.resmokelib.setup_multiversion.download import DownloadError
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import (
    SetupMultiversion,
    _DownloadOptions,
)
from buildscripts.resmokelib.symbolizer import Symbolizer
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path
from buildscripts.resmokelib.utils.otel_thread_pool_executor import OtelThreadPoolExecutor
from buildscripts.resmokelib.utils.otel_utils import get_default_current_span
from evergreen.task import Artifact, Task

_DEBUG_FILE_BASE_NAMES = ["mongo", "mongod", "mongos"]
TOOLCHAIN_ROOT = "/opt/mongodbtoolchain/v4"
TRACER = trace.get_tracer("resmoke")


def run_with_retries(
    root_logger: Logger, func: Callable[..., bool], timeout_secs: int, retry_secs: int, **kwargs
) -> bool:
    start_time = time.time()
    while True:
        try:
            if func(root_logger=root_logger, **kwargs):
                return True
        except Exception as ex:
            root_logger.error(f"Exception hit while running {func.__name__}")
            root_logger.error(ex)

        time_difference = time.time() - start_time
        if time_difference > timeout_secs:
            root_logger.error(
                f"Timeout hit for function {func.__name__} after {time_difference} seconds."
            )
            return False

        root_logger.error(f"Failed to run {func.__name__}, retrying in {retry_secs} seconds...")
        time.sleep(retry_secs)


@TRACER.start_as_current_span("core_analyzer.download_core_dumps")
def download_core_dumps(
    root_logger: Logger, task: Task, download_dir: str, debugger: Dumper, multiversion_versions: set
) -> bool:
    root_logger.info("Looking for core dumps")
    artifacts = task.artifacts
    core_dumps_found = 0
    core_dumps_dir = os.path.join(download_dir, "core-dumps")
    os.makedirs(core_dumps_dir, exist_ok=True)
    current_span = get_default_current_span()
    for artifact in artifacts:
        if not artifact.name.startswith("Core Dump"):
            continue

        file_name = artifact.url.split("/")[-1]
        extracted_name, _ = os.path.splitext(file_name)
        extract_path = os.path.join(core_dumps_dir, extracted_name)

        with TRACER.start_as_current_span(
            "core_analyzer.download_core_dump",
            attributes={
                "core_dump_file_name": file_name,
                "core_dump_file_url": artifact.url,
            },
        ) as core_dump_span:
            attempts = 0
            core_dump_span.set_status(StatusCode.OK)
            try:

                @retry(tries=3, delay=5)
                def download_core_dump(artifact: Artifact):
                    nonlocal attempts
                    attempts += 1
                    root_logger.info(f"Downloading core dump: {file_name}")
                    if os.path.exists(file_name):
                        os.remove(file_name)
                    urllib.request.urlretrieve(artifact.url, file_name)
                    root_logger.info(f"Extracting core dump: {file_name}")
                    if os.path.exists(extract_path):
                        os.remove(extract_path)
                    with gzip.open(file_name, "rb") as f_in:
                        with open(extract_path, "wb") as f_out:
                            shutil.copyfileobj(f_in, f_out)

                    core_dump_span.set_attributes(
                        {
                            "core_dump_compressed_size": os.path.getsize(file_name),
                            "core_dump_extracted_size": os.path.getsize(extract_path),
                            "core_dump_download_attempts": attempts,
                        }
                    )
                    root_logger.info(
                        f"Done extracting core dump {extracted_name} to {extract_path}"
                    )
                    os.remove(file_name)

                    _, bin_version = debugger.get_binary_from_core_dump(extract_path)
                    if bin_version:
                        multiversion_versions.add(bin_version)

                download_core_dump(artifact)
                core_dumps_found += 1

            except Exception as ex:
                root_logger.error(
                    "An error occured while trying to download and extract core dump %s",
                    extracted_name,
                )
                root_logger.error(ex)
                core_dump_span.set_status(StatusCode.ERROR, "Failed to download core dump.")
                core_dump_span.set_attributes(
                    {
                        "core_dump_download_attempts": attempts,
                        "core_dump_error": ex,
                    }
                )

    core_dump_directory_size = sum(f.stat().st_size for f in Path("./").glob("**/*") if f.is_file())
    current_span.set_attributes(
        {
            "core_dumps_dir": core_dumps_dir,
            "core_dump_directory_size": core_dump_directory_size,
        }
    )

    if not core_dumps_found:
        root_logger.error("No core dumps found")
        current_span.set_status(StatusCode.ERROR, description="No core dumps found")
        current_span.set_attributes(
            {
                "core_dumps_error": "No core dumps found",
            }
        )
        return False

    return True


@TRACER.start_as_current_span("core_analyzer.download_multiversion_artifact")
def download_multiversion_artifact(
    root_logger: Logger,
    version_id: str,
    variant: str,
    download_options: _DownloadOptions,
    download_dir: str,
    name: str,
    bin_version: str = None,
) -> bool:
    current_span = get_default_current_span(
        {"downloaded_artifact_type": name, "version": bin_version if bin_version else "current"}
    )
    try:
        root_logger.info("Downloading %s", name)
        multiversion_setup = SetupMultiversion(
            download_options=download_options,
            ignore_failed_push=True,
            link_dir=os.path.abspath(download_dir),
        )
        urlinfo = multiversion_setup.get_urls(version=version_id, buildvariant_name=variant)
        if bin_version:
            install_dir = os.path.abspath(os.path.join(download_dir, bin_version, "install"))
            os.makedirs(install_dir, exist_ok=True)
            multiversion_setup.download_and_extract_from_urls(
                urlinfo.urls, bin_suffix=bin_version, install_dir=install_dir, skip_symlinks=False
            )
        else:
            install_dir = os.path.abspath(os.path.join(download_dir, "install"))
            os.makedirs(install_dir, exist_ok=True)
            multiversion_setup.download_and_extract_from_urls(
                urlinfo.urls, bin_suffix=None, install_dir=install_dir, skip_symlinks=True
            )
        root_logger.info("Downloaded %s", name)
        return True
    except Exception as ex:
        root_logger.error("An error occured while trying to download %s", name)
        root_logger.error(ex)
        current_span.set_status(StatusCode.ERROR, f"Failed to download {name}")
        current_span.set_attribute("download_multiversion_artifact_error", ex)
        return False


@TRACER.start_as_current_span("core_analyzer.post_install_gdb_optimization")
def post_install_gdb_optimization(download_dir: str, root_looger: Logger):
    @TRACER.start_as_current_span("core_analyzer.post_install_gdb_optimization.add_index")
    def add_index(file_path: str):
        """Generate and add gdb-index to ELF binary."""
        current_span = get_default_current_span(
            {
                "file": file_path,
                "add_index_status": "success",
                "add_index_original_file_size": os.path.getsize(file_path),
            }
        )
        start_time = time.time()
        process = subprocess.run(
            [f"{TOOLCHAIN_ROOT}/bin/llvm-dwarfdump", "-r", "0", file_path],
            capture_output=True,
            text=True,
        )

        # it is normal for non debug binaries to fail this command
        # there also can be some python files in the bin dir that will fail
        if process.returncode != 0:
            current_span.set_attribute("add_index_status", "skipped")
            return

        # find dwarf version from output, it should always be present
        regex = re.search("version = 0x([0-9]{4}),", process.stdout)
        if not regex:
            current_span.set_status(StatusCode.ERROR, "Could not find dwarf version in file.")
            current_span.set_attributes(
                {
                    "add_index_status": "failed",
                    "add_index_error": "Could not find dwarf version in file",
                }
            )
            raise RuntimeError(f"Could not find dwarf version in file {file_path}")

        version = int(regex.group(1))
        target_dir = os.path.dirname(file_path)
        current_span.set_attribute("dwarf_version", version)

        try:
            # logic copied from https://sourceware.org/gdb/onlinedocs/gdb/Index-Files.html
            if version == 5:
                subprocess.run(
                    [
                        f"{TOOLCHAIN_ROOT}/bin/gdb",
                        "--batch-silent",
                        "--quiet",
                        "--nx",
                        "--eval-command",
                        f"save gdb-index -dwarf-5 {target_dir}",
                        file_path,
                    ],
                    check=True,
                )
                subprocess.run(
                    [
                        f"{TOOLCHAIN_ROOT}/bin/objcopy",
                        "--dump-section",
                        f".debug_str={file_path}.debug_str.new",
                        file_path,
                    ]
                )
                with open(f"{file_path}.debug_str", "r") as file1:
                    with open(f"{file_path}.debug_str.new", "a") as file2:
                        file2.write(file1.read())
                subprocess.run(
                    [
                        f"{TOOLCHAIN_ROOT}/bin/objcopy",
                        "--add-section",
                        f".debug_names={file_path}.debug_names",
                        "--set-section-flags",
                        ".debug_names=readonly",
                        "--update-section",
                        f".debug_str={file_path}.debug_str.new",
                        file_path,
                        file_path,
                    ],
                    check=True,
                )
                os.remove(f"{file_path}.debug_str.new")
                os.remove(f"{file_path}.debug_str")
                os.remove(f"{file_path}.debug_names")

            elif version == 4:
                subprocess.run(
                    [
                        f"{TOOLCHAIN_ROOT}/bin/gdb",
                        "--batch-silent",
                        "--quiet",
                        "--nx",
                        "--eval-command",
                        f"save gdb-index {target_dir}",
                        file_path,
                    ],
                    check=True,
                )
                subprocess.run(
                    [
                        f"{TOOLCHAIN_ROOT}/bin/objcopy",
                        "--add-section",
                        f".gdb_index={file_path}.gdb-index",
                        "--set-section-flags",
                        ".gdb_index=readonly",
                        file_path,
                        file_path,
                    ],
                    check=True,
                )
                os.remove(f"{file_path}.gdb-index")
            else:
                current_span.set_status(StatusCode.ERROR, f"Unsupported dwarf version: {version}")
                current_span.set_attributes(
                    {
                        "add_index_status": "failed",
                        "add_index_error": "Does not support dwarf version",
                    }
                )
                raise RuntimeError(f"Does not support dwarf version {version}")
        except Exception as ex:
            root_looger.exception("Failed to add gdb index to %s", file_path)
            current_span.set_status(StatusCode.ERROR, "Failed to add gdb index")
            current_span.set_attributes(
                {
                    "add_index_status": "failed",
                    "add_index_error": ex,
                }
            )
            return

        current_span.set_attribute("add_index_changed_file_size", os.path.getsize(file_path))

        root_looger.debug(
            "Finished creating gdb-index for %s in %s", file_path, (time.time() - start_time)
        )

    @TRACER.start_as_current_span("core_analyzer.post_install_gdb_optimization.recalc_debuglink")
    def recalc_debuglink(file_path: str):
        """
        Recalcuate the debuglink for ELF binaries.

        After creating the index file in a separate debug file, the debuglink CRC
        is no longer valid, this will simply recreate the debuglink and therefore
        update the CRC to match.
        """
        current_span = get_default_current_span(
            {"file": file_path, "recalc_debuglink_status": "success"}
        )

        process = subprocess.run(
            [f"{TOOLCHAIN_ROOT}/bin/eu-readelf", "-S", file_path], capture_output=True, text=True
        )
        if process.returncode != 0 or ".gnu_debuglink" not in process.stdout:
            current_span.set_attribute("recalc_debuglink_status", "skipped")
            return
        try:
            subprocess.run(
                [f"{TOOLCHAIN_ROOT}/bin/objcopy", "--remove-section", ".gnu_debuglink", file_path],
                check=True,
            )
            subprocess.run(
                [
                    f"{TOOLCHAIN_ROOT}/bin/objcopy",
                    "--add-gnu-debuglink",
                    f"{os.path.abspath(file_path)}.debug",
                    file_path,
                ],
                check=True,
            )
        except Exception as ex:
            root_looger.exception("Failed to recalculate debuglink")
            current_span.set_status(StatusCode.ERROR, "Failed to recalculate debuglink")
            current_span.set_attributes(
                {
                    "recalc_debuglink_status": "failed",
                    "recalc_debuglink_error": ex,
                }
            )
            return

        root_looger.debug("Finished recalculating the debuglink for %s", file_path)

    install_dir = os.path.join(download_dir, "install")
    lib_dir = os.path.join(install_dir, "dist-test", "lib")
    if not os.path.exists(lib_dir):
        return
    lib_files = [os.path.join(lib_dir, file_path) for file_path in os.listdir(lib_dir)]
    current_span = get_default_current_span()

    with OtelThreadPoolExecutor() as executor:
        with TRACER.start_as_current_span(
            "core_analyzer.post_install_gdb_optimization.add_indexes"
        ) as current_span:
            futures = []
            current_span.set_status(StatusCode.OK)
            for file_path in lib_files:
                # When we add the .gdb_index section to binaries being ran with gdb
                # it makes gdb not longer recognize them as the binary that generated
                # the core dumps
                futures.append(executor.submit(add_index, file_path=file_path))

            concurrent.futures.wait(futures)

        with TRACER.start_as_current_span(
            "core_analyzer.post_install_gdb_optimization.recalc_debuglink"
        ):
            futures = []
            current_span = get_default_current_span()
            for file_path in lib_files:
                # There will be no debuglinks in the separate debug files
                # We do not want to edit any of the binary files that gdb might directly run
                # so we skip the bin directory
                if file_path.endswith(".debug"):
                    continue
                futures.append(executor.submit(recalc_debuglink, file_path=file_path))
            concurrent.futures.wait(futures)


@TRACER.start_as_current_span("core_analyzer.download_task_artifacts")
def download_task_artifacts(
    root_logger: Logger,
    task_id: str,
    download_dir: str,
    debugger: Dumper,
    multiversion_dir: str,
    execution: Optional[int] = None,
    retry_secs: int = 10,
    download_timeout_secs: int = 30 * 60,
) -> bool:
    if os.path.exists(download_dir):
        # quick sanity check to ensure we don't delete a repo
        if os.path.exists(os.path.join(download_dir, ".git")):
            raise RuntimeError(f"Input dir cannot be a git repo: {download_dir}")

        shutil.rmtree(download_dir)
        root_logger.info(f"Deleted existing dir at {download_dir}")

    os.mkdir(download_dir)

    current_span = get_default_current_span({"download_task_id": task_id})
    evg_api = evergreen_conn.get_evergreen_api()
    if execution is not None:
        task_info = evg_api.task_by_id(task_id=task_id, execution=execution)
    else:
        task_info = evg_api.task_by_id(task_id)
    binary_download_options = _DownloadOptions(db=True, ds=False, da=False, dv=False)
    debugsymbols_download_options = _DownloadOptions(db=False, ds=True, da=False, dv=False)

    @retry(tries=3, delay=5)
    def get_multiversion_download_links(task: Task) -> Optional[dict]:
        for artifact in task.artifacts:
            if artifact.name != "Multiversion download links":
                continue

            with urllib.request.urlopen(artifact.url) as url:
                return json.load(url)
        return None

    # dictionary of versions and the information about where to download that version
    # The key is the version, if the key is an empty string that means master was downloaded
    multiversion_downloads = get_multiversion_download_links(task_info)

    # We support `skip_compile` tasks that download master instead of compiling it
    # For analysis on these tasks we should download the same binaries that the task downloaded
    if multiversion_downloads and "" in multiversion_downloads:
        version_downloads = multiversion_downloads[""]
        variant = version_downloads["evg_build_variant"]
        version_id = version_downloads["evg_version_id"]
    else:
        variant = task_info.build_variant
        version_id = task_info.version_id

    all_downloaded = True
    multiversion_versions = set()
    with OtelThreadPoolExecutor() as executor:
        futures = []
        futures.append(
            executor.submit(
                run_with_retries,
                root_logger=root_logger,
                func=download_core_dumps,
                timeout_secs=download_timeout_secs,
                retry_secs=retry_secs,
                task=task_info,
                download_dir=download_dir,
                debugger=debugger,
                multiversion_versions=multiversion_versions,
            )
        )
        futures.append(
            executor.submit(
                run_with_retries,
                func=download_multiversion_artifact,
                timeout_secs=download_timeout_secs,
                retry_secs=retry_secs,
                root_logger=root_logger,
                version_id=version_id,
                variant=variant,
                download_options=binary_download_options,
                download_dir=download_dir,
                name="binaries",
            )
        )
        futures.append(
            executor.submit(
                run_with_retries,
                func=download_multiversion_artifact,
                timeout_secs=download_timeout_secs,
                retry_secs=retry_secs,
                root_logger=root_logger,
                version_id=version_id,
                variant=variant,
                download_options=debugsymbols_download_options,
                download_dir=download_dir,
                name="debugsymbols",
            )
        )

        for future in concurrent.futures.as_completed(futures):
            if not future.result():
                current_span.set_status(StatusCode.ERROR, "Errors occured while fetching artifacts")
                current_span.set_attribute(
                    "download_task_artifacts_error", "Errors occured while fetching artifacts"
                )
                root_logger.error("Errors occured while fetching artifacts")
                all_downloaded = False
                break

    if multiversion_versions:
        if not multiversion_downloads:
            raise RuntimeError("Multiversion core dumps were found without download links.")

        with OtelThreadPoolExecutor() as executor:
            futures = []
            for version in multiversion_versions:
                version_downloads = multiversion_downloads[version]
                version_id = version_downloads["evg_version_id"]
                variant = version_downloads["evg_build_variant"]
                futures.append(
                    executor.submit(
                        run_with_retries,
                        func=download_multiversion_artifact,
                        timeout_secs=download_timeout_secs,
                        retry_secs=retry_secs,
                        root_logger=root_logger,
                        version_id=version_id,
                        variant=variant,
                        download_options=binary_download_options,
                        download_dir=multiversion_dir,
                        name=f"binaries-{version}",
                        bin_version=version,
                    )
                )
                futures.append(
                    executor.submit(
                        run_with_retries,
                        func=download_multiversion_artifact,
                        timeout_secs=download_timeout_secs,
                        retry_secs=retry_secs,
                        root_logger=root_logger,
                        version_id=version_id,
                        variant=variant,
                        download_options=debugsymbols_download_options,
                        download_dir=multiversion_dir,
                        name=f"debugsymbols-{version}",
                        bin_version=version,
                    )
                )

            for future in concurrent.futures.as_completed(futures):
                if not future.result():
                    current_span.set_status(
                        StatusCode.ERROR, "Errors occured while fetching old version artifacts"
                    )
                    current_span.set_attribute(
                        "download_task_artifacts_error",
                        "Errors occured while fetching old version artifacts",
                    )
                    root_logger.error("Errors occured while fetching old version artifacts")
                    all_downloaded = False
                    break

    if all_downloaded and sys.platform.startswith("linux"):
        post_install_gdb_optimization(download_dir, root_logger)

        for version in multiversion_versions:
            post_install_gdb_optimization(os.path.join(multiversion_dir, version), root_logger)

    return all_downloaded


def download_debug_symbols(
    root_logger, symbolizer: Symbolizer, retry_secs: int = 10, download_timeout_secs: int = 10 * 60
):
    """
    Extract debug symbols. Idempotent.

    :param root_logger: logger to use
    :param symbolizer: pre-configured instance of symbolizer for downloading symbols.
    :param retry_secs: seconds before retrying to download symbols
    :param download_timeout_secs: timeout in seconds before failing to download
    :return: None
    """

    # Check if the files are already there. They would be on *SAN builds.
    sym_files = _get_symbol_files()

    if len(sym_files) >= len(_DEBUG_FILE_BASE_NAMES):
        root_logger.info(
            "Skipping downloading debug symbols as there are already symbol files present"
        )
        return

    while True:
        try:
            symbolizer.execute()
            root_logger.info("Debug symbols successfully downloaded")
            break
        except (tarfile.ReadError, DownloadError):
            root_logger.warn(
                "Debug symbols unavailable after %s secs, retrying in %s secs, waiting for a total of %s secs",
                compare_start_time(time.time()),
                retry_secs,
                download_timeout_secs,
            )
        time.sleep(retry_secs)

        if compare_start_time(time.time()) > download_timeout_secs:
            root_logger.warn(
                "Debug-symbols archive-file does not exist after %s secs; "
                "Hang-Analyzer may not complete successfully.",
                download_timeout_secs,
            )
            break


def _get_symbol_files():
    out = []
    for ext in ["debug", "dSYM", "pdb"]:
        for file in _DEBUG_FILE_BASE_NAMES:
            haystack = build_hygienic_bin_path(child="{file}.{ext}".format(file=file, ext=ext))
            for needle in glob.glob(haystack):
                out.append((needle, os.path.join(os.getcwd(), os.path.basename(needle))))
    return out
