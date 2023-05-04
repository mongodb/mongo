"""Symbolize stacktraces inside test logs."""
from __future__ import annotations

import os
import subprocess
import sys
import time
from datetime import timedelta
from threading import Lock

from typing import List, Optional, NamedTuple, Set

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.testing.testcases.interface import TestCase

# This lock prevents different resmoke jobs from symbolizing stacktraces concurrently,
# which includes downloading the debug symbols, that can be reused by other resmoke jobs
_lock = Lock()

STACKTRACE_FILE_EXTENSION = ".stacktrace"
SYMBOLIZE_RETRY_TIMEOUT_SECS = timedelta(minutes=4).total_seconds()
PROCESSED_FILES_LIST_FILE_PATH = "symbolizer-processed-files.txt"  # noqa


class ResmokeSymbolizerConfig(NamedTuple):
    """
    Resmoke symbolizer config.

    * evg_task_id: evergreen task ID resmoke runs on
    * client_id: symbolizer client ID
    * client_secret: symbolizer client secret
    """

    evg_task_id: Optional[str]
    client_id: Optional[str]
    client_secret: Optional[str]

    @classmethod
    def from_resmoke_config(cls) -> ResmokeSymbolizerConfig:
        """
        Make resmoke symbolizer config from a global resmoke config.

        :return: resmoke symbolizer config
        """
        return cls(
            evg_task_id=_config.EVERGREEN_TASK_ID,
            client_id=_config.SYMBOLIZER_CLIENT_ID,
            client_secret=_config.SYMBOLIZER_CLIENT_SECRET,
        )

    @staticmethod
    def is_windows() -> bool:
        """
        Whether we are on Windows.

        :return: True if on Windows
        """
        return sys.platform == "win32" or sys.platform == "cygwin"

    @staticmethod
    def is_macos() -> bool:
        """
        Whether we are on MacOS.

        :return: True if on MacOS.
        """
        return sys.platform == "darwin"


class ResmokeSymbolizer:
    """Symbolize stacktraces inside test logs."""

    def __init__(self, config: Optional[ResmokeSymbolizerConfig] = None,
                 symbolizer_service: Optional[SymbolizerService] = None,
                 file_service: Optional[FileService] = None):
        """Initialize instance."""

        self.config = config if config is not None else ResmokeSymbolizerConfig.from_resmoke_config(
        )
        self.symbolizer_service = symbolizer_service if symbolizer_service is not None else SymbolizerService(
        )
        self.file_service = file_service if file_service is not None else FileService(
            PROCESSED_FILES_LIST_FILE_PATH)

    def symbolize_test_logs(self, test: TestCase,
                            symbolize_retry_timeout: float = SYMBOLIZE_RETRY_TIMEOUT_SECS) -> None:
        """
        Perform all necessary actions to symbolize and write output to test logs.

        :param test: resmoke test case
        :param symbolize_retry_timeout: the timeout for symbolizer retries
        """
        if not self.should_symbolize(test):
            return

        dbpath = self.get_stacktrace_dir(test)
        if dbpath is None:
            return

        with _lock:
            test.logger.info("Looking for stacktrace files in '%s'", dbpath)
            files = self.collect_stacktrace_files(dbpath)
            if not files:
                test.logger.info("No failure logs/stacktrace files found, skipping symbolization")
                return

            test.logger.info("Found stacktrace files. \nBEGIN Symbolization")
            test.logger.info("Stacktrace files: %s", files)

            start_time = time.perf_counter()
            for file_path in files:
                test.logger.info("Working on: %s", file_path)
                symbolizer_script_timeout = int(symbolize_retry_timeout -
                                                (time.perf_counter() - start_time))
                symbolized_out = self.symbolizer_service.run_symbolizer_script(
                    file_path, symbolizer_script_timeout)
                test.logger.info(symbolized_out)
                if time.perf_counter() - start_time > symbolize_retry_timeout:
                    break

            # To avoid performing the same actions on these files again, we mark them as processed
            self.file_service.add_to_processed_files(files)
            self.file_service.write_processed_files(PROCESSED_FILES_LIST_FILE_PATH)

            test.logger.info("\nEND Symbolization \nSymbolization process completed. ")

    def should_symbolize(self, test: TestCase) -> bool:
        """
        Check whether we should perform symbolization process.

        :param test: resmoke test case
        :return: whether we should symbolize
        """
        if self.config.evg_task_id is None:
            test.logger.info("Not running in Evergreen, skipping symbolization")
            return False

        if self.config.client_id is None or self.config.client_secret is None:
            test.logger.info("Symbolizer client secret and/or client ID are absent,"
                             " skipping symbolization")
            return False

        if self.config.is_windows():
            test.logger.info("Running on Windows, skipping symbolization")
            return False

        if self.config.is_macos():
            test.logger.info("Running on MacOS, skipping symbolization")
            return False

        return True

    def get_stacktrace_dir(self, test: TestCase) -> Optional[str]:
        """
        Get dbpath from test case.

        :param test: resmoke test case
        :return: dbpath or None
        """
        if not hasattr(test, "fixture") or test.fixture is None:
            test.logger.info("Test fixture is not available, could not get dbpath")
            return None

        dbpath = test.fixture.get_dbpath_prefix()
        if not self.file_service.check_path_exists(dbpath):
            test.logger.info("dbpath '%s' directory not found", dbpath)
            return None

        return dbpath

    def collect_stacktrace_files(self, dir_path: str) -> List[str]:
        """
        Collect all stacktrace files which are not empty and return their full paths.

        :param dir_path: directory to look into
        :return: list of stacktrace files paths
        """

        files = self.file_service.find_all_children_recursively(dir_path)
        files = self.file_service.filter_by_extension(files, STACKTRACE_FILE_EXTENSION)
        files = self.file_service.filter_out_empty_files(files)
        files = self.file_service.filter_out_non_files(files)
        files = self.file_service.filter_out_already_processed_files(files)

        return files


class FileService:
    """A service for working with files."""

    def __init__(self, processed_files_list_path: str = PROCESSED_FILES_LIST_FILE_PATH):
        """Initialize FileService instance."""
        self._processed_files = self.load_processed_files(processed_files_list_path)

    @staticmethod
    def load_processed_files(file_path: str) -> Set[str]:
        """
        Load processed files info from a file.

        :param: path to a file where we store processed files info.
        """
        if os.path.exists(file_path):
            with open(file_path, "r") as file:
                return {line for line in set(file.readlines()) if line}
        return set()

    def add_to_processed_files(self, files: List[str]) -> None:
        """
        Bulk add to collection of processed files.

        :param files: files to add to processed files collection
        :return: None
        """
        for file in files:
            self._processed_files.add(file)

    def write_processed_files(self, file_path: str) -> None:
        """
        Write processed files info to a file.

        :param file_path: path to a file where we store processed files info
        :return: None
        """
        with open(file_path, "w") as file:
            file.write("\n".join(self._processed_files))

    def is_processed(self, file: str) -> bool:
        """
        Check if file is already processed or not.

        :param file: file path
        :return: whether the file is already processed or not
        """
        return file in self._processed_files

    @staticmethod
    def find_all_children_recursively(dir_path: str) -> List[str]:
        """
        Find all children files in directory recursively.

        :param dir_path: directory path
        :return: list of all children files
        """
        children_in_dir = []
        for parent, _, children in os.walk(dir_path):
            children_in_dir.extend(os.path.join(parent, child) for child in children)
        return children_in_dir

    @staticmethod
    def filter_by_extension(files: List[str], extension: str) -> List[str]:
        """
        Filter files by extension.

        :param files: list of file paths
        :param extension: file extension
        :return: filtered list of file paths
        """
        return [f for f in files if f.endswith(extension)]

    @staticmethod
    def filter_out_non_files(files: List[str]) -> List[str]:
        """
        Filter out non files.

        :param files: list of paths
        :return: filtered list of file paths
        """
        return [f for f in files if os.path.isfile(f)]

    def filter_out_already_processed_files(self, files: List[str]):
        """
        Filter out already processed files.

        :param files: list of file paths
        :return: non-processed files
        """
        return [f for f in files if not self.is_processed(f)]

    @staticmethod
    def filter_out_empty_files(files: List[str]) -> List[str]:
        """
        Filter our files that are empty.

        :param files: list of paths
        :return: Non-empty files
        """
        filtered_files = []
        for file in files:
            try:
                if not os.stat(file).st_size == 0:
                    filtered_files.append(file)
            except FileNotFoundError:
                pass
        return filtered_files

    @staticmethod
    def check_path_exists(path: str) -> bool:
        """
        Check that file or directory exists.

        :param path: file or directory path
        :return: whether path exists
        """
        return os.path.exists(path)


class SymbolizerService:
    """Wrapper around symbolizer script."""

    @staticmethod
    def run_symbolizer_script(full_file_path: str, retry_timeout_secs: int) -> str:
        """
        Symbolize given file and return symbolized output as string.

        :param full_file_path: stacktrace file path
        :param retry_timeout_secs: the timeout for symbolizer to retry
        :return: symbolized output as string
        """

        symbolizer_args = [
            "db-contrib-tool",
            "symbolize",
            "--client-secret",
            _config.SYMBOLIZER_CLIENT_SECRET,
            "--client-id",
            _config.SYMBOLIZER_CLIENT_ID,
            "--total-seconds-for-retries",
            str(retry_timeout_secs),
        ]

        with open(full_file_path) as file_obj:
            symbolizer_process = subprocess.Popen(args=symbolizer_args, close_fds=True,
                                                  stdin=file_obj, stdout=subprocess.PIPE,
                                                  stderr=subprocess.STDOUT)

        try:
            output, _ = symbolizer_process.communicate(timeout=retry_timeout_secs)
        except subprocess.TimeoutExpired:
            symbolizer_process.kill()
            output, _ = symbolizer_process.communicate()

        return output.strip().decode()
