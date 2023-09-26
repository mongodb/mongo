"""Extracts `mongo-debugsymbols.tgz` in an idempotent manner for performance."""
import concurrent.futures
import glob
import gzip
from logging import Logger
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from typing import Callable, Optional
import urllib.request

from retry import retry

from buildscripts.resmokelib.setup_multiversion.download import DownloadError
from buildscripts.resmokelib.run import compare_start_time
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import _DownloadOptions, SetupMultiversion
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path
from buildscripts.resmokelib.symbolizer import Symbolizer
from evergreen.task import Task, Artifact

_DEBUG_FILE_BASE_NAMES = ['mongo', 'mongod', 'mongos']
TOOLCHAIN_ROOT = "/opt/mongodbtoolchain/v4"


def run_with_retries(root_logger: Logger, func: Callable[..., bool], timeout_secs: int,
                     retry_secs: int, **kwargs) -> bool:
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
                f"Timeout hit for function {func.__name__} after {time_difference} seconds.")
            return False

        root_logger.error(f"Failed to run {func.__name__}, retrying in {retry_secs} seconds...")
        time.sleep(retry_secs)


def download_core_dumps(root_logger: Logger, task: Task, download_dir: str) -> bool:
    root_logger.info("Looking for core dumps")
    artifacts = task.artifacts
    core_dumps_found = False
    core_dumps_dir = os.path.join(download_dir, "core-dumps")
    os.makedirs(core_dumps_dir, exist_ok=True)
    for artifact in artifacts:
        if not artifact.name.startswith("Core Dump"):
            continue

        file_name = artifact.url.split("/")[-1]
        extracted_name, _ = os.path.splitext(file_name)
        extract_path = os.path.join(core_dumps_dir, extracted_name)
        try:

            @retry(tries=3, delay=5)
            def download_core_dump(artifact: Artifact):
                root_logger.info(f"Downloading core dump: {file_name}")
                if os.path.exists(file_name):
                    os.remove(file_name)
                urllib.request.urlretrieve(artifact.url, file_name)
                root_logger.info(f"Extracting core dump: {file_name}")
                if os.path.exists(extract_path):
                    os.remove(extract_path)
                with gzip.open(file_name, 'rb') as f_in:
                    with open(extract_path, 'wb') as f_out:
                        shutil.copyfileobj(f_in, f_out)
                root_logger.info(f"Done extracting core dump {extracted_name} to {extract_path}")
                os.remove(file_name)

            download_core_dump(artifact)
            core_dumps_found = True

        except Exception as ex:
            root_logger.error("An error occured while trying to download and extract core dump %s",
                              extracted_name)
            root_logger.error(ex)

    if not core_dumps_found:
        root_logger.error("No core dumps found")
        return False

    return True


def download_multiversion_artifact(root_logger: Logger, task: Task,
                                   download_options: _DownloadOptions, download_dir: str,
                                   name: str) -> bool:
    try:
        root_logger.info("Downloading %s", name)
        multiversion_setup = SetupMultiversion(download_options=download_options,
                                               ignore_failed_push=True)
        urlinfo = multiversion_setup.get_urls(version=task.version_id,
                                              buildvariant_name=task.build_variant)
        install_dir = os.path.join(download_dir, "install")
        os.makedirs(install_dir, exist_ok=True)
        multiversion_setup.download_and_extract_from_urls(
            urlinfo.urls, bin_suffix=None, install_dir=install_dir, skip_symlinks=True)
        root_logger.info("Downloaded %s", name)
        return True
    except Exception as ex:
        root_logger.error("An error occured while trying to download %s", name)
        root_logger.error(ex)
        return False


def post_install_gdb_optimization(download_dir: str, root_looger: Logger):
    def add_index(file_path: str):
        """Generate and add gdb-index to ELF binary."""
        start_time = time.time()
        process = subprocess.run([f"{TOOLCHAIN_ROOT}/bin/llvm-dwarfdump", "-r", "0", file_path],
                                 capture_output=True, text=True)

        # it is normal for non debug binaries to fail this command
        # there also can be some python files in the bin dir that will fail
        if process.returncode != 0:
            return

        # find dwarf version from output, it should always be present
        regex = re.search("version = 0x([0-9]{4}),", process.stdout)
        if not regex:
            raise RuntimeError(f"Could not find dwarf version in file {file_path}")

        version = int(regex.group(1))
        target_dir = os.path.dirname(file_path)

        # logic copied from https://sourceware.org/gdb/onlinedocs/gdb/Index-Files.html
        if version == 5:
            subprocess.run([
                f"{TOOLCHAIN_ROOT}/bin/gdb", "--batch-silent", "--quiet", "--nx", "--eval-command",
                f"save gdb-index -dwarf-5 {target_dir}", file_path
            ], check=True)
            subprocess.run([
                f"{TOOLCHAIN_ROOT}/bin/objcopy", "--dump-section",
                f".debug_str={file_path}.debug_str.new", file_path
            ])
            with open(f"{file_path}.debug_str", "r") as file1:
                with open(f"{file_path}.debug_str.new", "a") as file2:
                    file2.write(file1.read())
            subprocess.run([
                f"{TOOLCHAIN_ROOT}/bin/objcopy", "--add-section",
                f".debug_names={file_path}.debug_names", "--set-section-flags",
                ".debug_names=readonly", "--update-section",
                f".debug_str={file_path}.debug_str.new", file_path, file_path
            ], check=True)
            os.remove(f"{file_path}.debug_str.new")
            os.remove(f"{file_path}.debug_str")
            os.remove(f"{file_path}.debug_names")

        elif version == 4:
            subprocess.run([
                f"{TOOLCHAIN_ROOT}/bin/gdb", "--batch-silent", "--quiet", "--nx", "--eval-command",
                f"save gdb-index {target_dir}", file_path
            ], check=True)
            subprocess.run([
                f"{TOOLCHAIN_ROOT}/bin/objcopy", "--add-section",
                f".gdb_index={file_path}.gdb-index", "--set-section-flags", ".gdb_index=readonly",
                file_path, file_path
            ], check=True)
            os.remove(f"{file_path}.gdb-index")
        else:
            raise RuntimeError(f"Does not support dwarf version {version}")

        root_looger.debug("Finished creating gdb-index for %s in %s", file_path,
                          (time.time() - start_time))

    def recalc_debuglink(file_path: str):
        """
        Recalcuate the debuglink for ELF binaries.
        
        After creating the index file in a separate debug file, the debuglink CRC
        is no longer valid, this will simply recreate the debuglink and therefore
        update the CRC to match.
        """
        process = subprocess.run([f"{TOOLCHAIN_ROOT}/bin/eu-readelf", "-S", file_path],
                                 capture_output=True, text=True)
        if process.returncode != 0:
            return

        if ".gnu_debuglink" not in process.stdout:
            return

        subprocess.run(
            [f"{TOOLCHAIN_ROOT}/bin/objcopy", "--remove-section", ".gnu_debuglink", file_path],
            check=True)
        subprocess.run([
            f"{TOOLCHAIN_ROOT}/bin/objcopy", "--add-gnu-debuglink",
            f"{os.path.abspath(file_path)}.debug", file_path
        ], check=True)

        root_looger.debug("Finished recalculating the debuglink for %s", file_path)

    install_dir = os.path.join(download_dir, "install")
    lib_dir = os.path.join(install_dir, "dist-test", "lib")
    if not os.path.exists(lib_dir):
        return
    lib_files = [os.path.join(lib_dir, file_path) for file_path in os.listdir(lib_dir)]

    with concurrent.futures.ThreadPoolExecutor() as executor:
        futures = []
        for file_path in lib_files:
            # When we add the .gdb_index section to binaries being ran with gdb
            # it makes gdb not longer recognize them as the binary that generated
            # the core dumps
            futures.append(executor.submit(add_index, file_path=file_path))

        concurrent.futures.wait(futures)
        futures = []
        for file_path in lib_files:
            # There will be no debuglinks in the separate debug files
            # We do not want to edit any of the binary files that gdb might directly run
            # so we skip the bin directory
            if file_path.endswith(".debug"):
                continue
            futures.append(executor.submit(recalc_debuglink, file_path=file_path))
        concurrent.futures.wait(futures)


def download_task_artifacts(root_logger: Logger, task_id: str, download_dir: str,
                            execution: Optional[int] = None, retry_secs: int = 10,
                            download_timeout_secs: int = 30 * 60) -> bool:
    if os.path.exists(download_dir):
        # quick sanity check to ensure we don't delete a repo
        if os.path.exists(os.path.join(download_dir, ".git")):
            raise RuntimeError(f"Input dir cannot be a git repo: {download_dir}")

        shutil.rmtree(download_dir)
        root_logger.info(f"Deleted existing dir at {download_dir}")

    os.mkdir(download_dir)

    evg_api = evergreen_conn.get_evergreen_api()
    if execution is not None:
        task_info = evg_api.task_by_id(task_id=task_id, execution=execution)
    else:
        task_info = evg_api.task_by_id(task_id)
    binary_download_options = _DownloadOptions(db=True, ds=False, da=False, dv=False)
    debugsymbols_download_options = _DownloadOptions(db=False, ds=True, da=False, dv=False)

    all_downloaded = True
    with concurrent.futures.ThreadPoolExecutor() as executor:
        futures = []
        futures.append(
            executor.submit(run_with_retries, root_logger=root_logger, func=download_core_dumps,
                            timeout_secs=download_timeout_secs, retry_secs=retry_secs,
                            task=task_info, download_dir=download_dir))
        futures.append(
            executor.submit(run_with_retries, func=download_multiversion_artifact,
                            timeout_secs=download_timeout_secs, retry_secs=retry_secs,
                            root_logger=root_logger, task=task_info,
                            download_options=binary_download_options, download_dir=download_dir,
                            name="binaries"))
        futures.append(
            executor.submit(run_with_retries, func=download_multiversion_artifact,
                            timeout_secs=download_timeout_secs, retry_secs=retry_secs,
                            root_logger=root_logger, task=task_info,
                            download_options=debugsymbols_download_options,
                            download_dir=download_dir, name="debugsymbols"))

        for future in concurrent.futures.as_completed(futures):
            if not future.result():
                root_logger.error("Errors occured while fetching artifacts")
                all_downloaded = False
                break

    if all_downloaded and sys.platform.startswith("linux"):
        post_install_gdb_optimization(download_dir, root_logger)

    return all_downloaded


def download_debug_symbols(root_logger, symbolizer: Symbolizer, retry_secs: int = 10,
                           download_timeout_secs: int = 10 * 60):
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
            "Skipping downloading debug symbols as there are already symbol files present")
        return

    while True:
        try:
            symbolizer.execute()
            root_logger.info("Debug symbols successfully downloaded")
            break
        except (tarfile.ReadError, DownloadError):
            root_logger.warn(
                "Debug symbols unavailable after %s secs, retrying in %s secs, waiting for a total of %s secs",
                compare_start_time(time.time()), retry_secs, download_timeout_secs)
        time.sleep(retry_secs)

        if compare_start_time(time.time()) > download_timeout_secs:
            root_logger.warn(
                'Debug-symbols archive-file does not exist after %s secs; '
                'Hang-Analyzer may not complete successfully.', download_timeout_secs)
            break


def _get_symbol_files():
    out = []
    for ext in ['debug', 'dSYM', 'pdb']:
        for file in _DEBUG_FILE_BASE_NAMES:
            haystack = build_hygienic_bin_path(child='{file}.{ext}'.format(file=file, ext=ext))
            for needle in glob.glob(haystack):
                out.append((needle, os.path.join(os.getcwd(), os.path.basename(needle))))
    return out
