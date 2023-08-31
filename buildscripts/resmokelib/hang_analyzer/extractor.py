"""Extracts `mongo-debugsymbols.tgz` in an idempotent manner for performance."""
import concurrent.futures
import glob
import gzip
from logging import Logger
import os
import shutil
import tarfile
import time
from typing import Callable
import urllib.request

from buildscripts.resmokelib.setup_multiversion.download import DownloadError
from buildscripts.resmokelib.run import compare_start_time
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import _DownloadOptions, SetupMultiversion
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path
from buildscripts.resmokelib.symbolizer import Symbolizer
from evergreen.task import Task

_DEBUG_FILE_BASE_NAMES = ['mongo', 'mongod', 'mongos']


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
    os.mkdir(core_dumps_dir)
    try:
        for artifact in artifacts:
            if not artifact.name.startswith("Core Dump"):
                continue

            file_name = artifact.url.split("/")[-1]
            extracted_name, _ = os.path.splitext(file_name)
            root_logger.info(f"Downloading core dump: {file_name}")
            urllib.request.urlretrieve(artifact.url, file_name)
            root_logger.info(f"Extracting core dump: {file_name}")
            with gzip.open(file_name, 'rb') as f_in:
                extract_path = os.path.join(core_dumps_dir, extracted_name)
                with open(extract_path, 'wb') as f_out:
                    shutil.copyfileobj(f_in, f_out)
            root_logger.info(f"Done extracting core dump {extracted_name} to {extract_path}")
            core_dumps_found = True
            os.remove(file_name)

    except Exception as ex:
        root_logger.error("An error occured while trying to download core dumps")
        root_logger.error(ex)
        return False

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
        multiversion_setup.download_and_extract_from_urls(urlinfo.urls, bin_suffix=None,
                                                          install_dir=install_dir)
        root_logger.info("Downloaded %s", name)
        return True
    except Exception as ex:
        root_logger.error("An error occured while trying to download %s", name)
        root_logger.error(ex)
        return False


def download_task_artifacts(root_logger: Logger, task_id: str, download_dir: str,
                            retry_secs: int = 10, download_timeout_secs: int = 20 * 60) -> bool:
    if os.path.exists(download_dir):
        # quick sanity check to ensure we don't delete a repo
        if os.path.exists(os.path.join(download_dir, ".git")):
            raise RuntimeError(f"Input dir cannot be a git repo: {download_dir}")

        shutil.rmtree(download_dir)
        root_logger.info(f"Deleted existing dir at {download_dir}")

    os.mkdir(download_dir)

    evg_api = evergreen_conn.get_evergreen_api()
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
