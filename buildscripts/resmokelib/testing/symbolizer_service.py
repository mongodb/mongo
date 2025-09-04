"""Symbolize stacktraces inside test logs."""

from __future__ import annotations

import ast
import json
import os
import subprocess
import sys
import time
from datetime import timedelta
from threading import Lock
from typing import Any, List, NamedTuple, Optional, Set

from opentelemetry import trace

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.flags import HANG_ANALYZER_CALLED
from buildscripts.resmokelib.testing.testcases.interface import TestCase
from buildscripts.util.read_config import read_config_file

# This lock prevents different resmoke jobs from symbolizing stacktraces concurrently,
# which includes downloading the debug symbols, that can be reused by other resmoke jobs
_symbolizer_lock = Lock()

# A lock for avoiding concurrent access to PROCESSED_FILES_LIST_FILE_PATH
_processed_files_lock = Lock()

STACKTRACE_FILE_EXTENSION = ".stacktrace"
SYMBOLIZE_RETRY_TIMEOUT_SECS = timedelta(minutes=4).total_seconds()
PROCESSED_FILES_LIST_FILE_PATH = "symbolizer-processed-files.txt"  # noqa
TRACER = trace.get_tracer("resmoke")

BACKTRACE_KEY = "backtrace"
PROCESS_INFO_KEY = "processInfo"

UNSYMBOLIZED_STACKTRACE_TXT = "unsymbolized_stacktraces.txt"
UNSYMBOLIZED_STACKTRACE_JSON = "unsymbolized_stacktraces.json"
UNSYMBOLIZED_STACKTRACE_INSTRUCTIONS = "unsymbolized_stacktrace_instructions.txt"


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
    skip_symbolization: bool

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
            skip_symbolization=_config.SKIP_SYMBOLIZATION,
        )

    @staticmethod
    def is_windows() -> bool:
        """
        Whether we are on Windows.

        :return: True if on Windows
        """
        return sys.platform in ("win32", "cygwin")

    @staticmethod
    def is_macos() -> bool:
        """
        Whether we are on MacOS.

        :return: True if on MacOS.
        """
        return sys.platform == "darwin"


class ResmokeSymbolizer:
    """Symbolize stacktraces inside test logs."""

    def __init__(
        self,
        config: Optional[ResmokeSymbolizerConfig] = None,
        symbolizer_service: Optional[SymbolizerService] = None,
        file_service: Optional[FileService] = None,
    ):
        """Initialize instance."""

        self.config = (
            config if config is not None else ResmokeSymbolizerConfig.from_resmoke_config()
        )
        self.symbolizer_service = (
            symbolizer_service if symbolizer_service is not None else SymbolizerService()
        )
        self.file_service = (
            file_service
            if file_service is not None
            else FileService(PROCESSED_FILES_LIST_FILE_PATH)
        )

    def get_unsymbolized_stacktrace(
        self,
        test: TestCase,
    ):
        """
        Perform all necessary actions to obtain the unsymbolized stack trace.
        If we skip symbolization, steps on how to manually symbolize the unsymbolized stacktraces are provided in Evergreen
        If we do not skip symbolization, the unsymbolized stacktraces are automatically symbolized and output

        :param test: resmoke test case
        """
        if test.return_code == 0:
            test.logger.info("Test succeeded, skipping symbolization")
            return

        dbpath = self.get_stacktrace_dir(test)
        if dbpath is None:
            return

        test.logger.info("Looking for stacktrace files in '%s'", dbpath)
        files = self.collect_stacktrace_files(dbpath)
        test.logger.info("Found stacktrace files: %s", files)

        if not files:
            test.logger.info(
                "No failure logs/stacktrace files found during unsymbolized stack trace acquisition"
            )
            return

        if not _config.EVERGREEN_TASK_ID:
            return

        self.file_service.add_to_processed_files(files)
        self.file_service.write_processed_files(PROCESSED_FILES_LIST_FILE_PATH)
        data = self.get_unsymbolized_stacktrace_data(test, files)
        self.make_symbolization_instructions_or_symbolize(test, data, files)

    def get_unsymbolized_stacktrace_data(self, test: TestCase, files: list[str]) -> dict:
        """
        Reads each file containing unsymbolized stacktraces and stores its content.
        In each entry, the original name of the file and the test associated with the stacktrace is also stored.
        These results are then written to a file posted on Evergreen.

        :param test: resmoke test case
        :param files: unsymbolized stacktrace file names
        :return: dictionary (each entry is the unsymbolized stacktrace file name, the stacktrace contents, and the test name)
        """
        with _symbolizer_lock:
            if not os.path.exists(UNSYMBOLIZED_STACKTRACE_JSON):
                data = {"unsymbolized_stacktraces": []}
            else:
                try:
                    with open(UNSYMBOLIZED_STACKTRACE_JSON, "r") as file:
                        data = json.load(file)
                except Exception as ex:
                    test.logger.info(f"unable to read existing unsymbolized_stacktraces file: {ex}")

            for f in files:
                unsymbolized_content_dict = {}
                try:
                    with open(f, "r") as file:
                        unsymbolized_content = ','.join([line.rstrip('\n') for line in file])
                        unsymbolized_content_dict = ast.literal_eval(unsymbolized_content)
                except Exception:
                    test.logger.error(f"Failed to parse stacktrace file {f}", exc_info=1)

                unsymbolized_stacktrace_details = {
                    "name": f,
                    "unsymbolized_stacktrace": unsymbolized_content_dict,
                    "test_name": test.long_name(),
                }
                data["unsymbolized_stacktraces"].append(unsymbolized_stacktrace_details)

                with open(UNSYMBOLIZED_STACKTRACE_TXT, "a") as outfile:
                    outfile.write(f"name: {f}\n\n")
                    outfile.write(
                        f"unsymbolized_stacktrace: {json.dumps(unsymbolized_content_dict)}\n\n"
                    )
                    outfile.write(f"test_name: {test.long_name()}\n\n")

                    outfile.write("\n----------------------------------\n")

        return data

    def make_symbolization_instructions_or_symbolize(
        self, test: TestCase, data: dict, files: list[str]
    ):
        """
        If we skip symbolization, produce steps on how to manually symbolize the unsymbolized stacktraces are provided in Evergreen.
        If we do not skip symbolization, the unsymbolized stacktraces are automatically symbolized and output by logs statements.

        :param test: resmoke test case
        :param data
        :param files: unsymbolized stacktrace file names

        """
        with _symbolizer_lock:
            if len(data.get("unsymbolized_stacktraces")) > 0:
                try:
                    with open(UNSYMBOLIZED_STACKTRACE_JSON, "w") as file:
                        json.dump(data, file, indent=4)
                except Exception as ex:
                    test.logger.info(f"unable to write to file: {ex}")

                if self.config.skip_symbolization:
                    self.create_unsymbolized_instructions(
                        test,
                        data.get("unsymbolized_stacktraces"),
                    )
                else:
                    self.symbolize_stacktraces(test, files)

    def symbolize_stacktraces(
        self,
        test: TestCase,
        files: List[str],
        symbolize_retry_timeout: float = SYMBOLIZE_RETRY_TIMEOUT_SECS,
    ) -> None:
        """
        Perform all necessary actions to symbolize the provided stack traces

        :param test: resmoke test case
        :param files: unsymbolized stacktrace file names
        :param symbolize_retry_timeout: the timeout for symbolizer retries
        """
        if not self.should_symbolize(test):
            return

        with TRACER.start_as_current_span("stack_trace_symbolization"):
            test.logger.info("\nBEGIN Symbolization")

            start_time = time.perf_counter()
            for file_path in files:
                test.logger.info("Working on: %s", file_path)
                symbolizer_script_timeout = int(
                    symbolize_retry_timeout - (time.perf_counter() - start_time)
                )
                symbolized_out = self.symbolizer_service.run_symbolizer_script(
                    file_path, symbolizer_script_timeout
                )
                test.logger.info(symbolized_out)
                if time.perf_counter() - start_time > symbolize_retry_timeout:
                    break

        test.logger.info("\nEND Symbolization")
        test.logger.info("Symbolization process completed.")

    def create_unsymbolized_instructions(
        self,
        test: TestCase,
        unsymbolized_stacktraces_info: List[dict],
        expansions_file: str = "../expansions.yml",
    ):
        """
        Creates steps on how to manually symbolize provided unsymbolized stacktraces

        :param unsymbolized_stacktrace_instructions: .txt file to write steps to
        :param test: resmoke test case
        :param unsymbolized_stacktraces_info: Unsymbolized stacktraces along with the file name and test name
        :param expansions_file: Location of evergreen expansions file

        """
        if not _config.EVERGREEN_TASK_ID:
            test.logger.info(
                "Skipping local symbolization instructions because evergreen task id was not provided."
            )
            return

        expansions = read_config_file(expansions_file)
        compile_variant = expansions.get("compile_variant")
        build_variant = expansions.get("build_variant")

        missing_keys = []
        for entry in unsymbolized_stacktraces_info:
            unsymbolized_stacktrace = entry["unsymbolized_stacktrace"]
            found_backtrace = self.get_value_recursively(unsymbolized_stacktrace, BACKTRACE_KEY)
            found_process_info = self.get_value_recursively(
                unsymbolized_stacktrace, PROCESS_INFO_KEY
            )
            if not (found_backtrace and found_process_info):
                missing_keys.append((entry["name"], entry["test_name"]))
        unsymbolizable_stacktrace_comment = (
            self.show_unsymbolizable_stacktraces(missing_keys) if missing_keys else None
        )

        content = f"""
There should be a file attached to this evergreen task labeled, "Unsymbolized Stack Traces - Execution" and then some execution value.
To symbolize these stack traces, you should use the CLI of db-contrib-tools for symbolization.
{unsymbolizable_stacktrace_comment if unsymbolizable_stacktrace_comment else "All of the stacktraces are eligible to be symbolized in the Unsymbolized Stacktrace file."}
Specifically, Create a local json file for each of the stacktraces (the content after the header "unsymbolized_stacktrace").
    WARNING: This json file must have the stacktrace all on one line for parsing to work properly.
    Also, please ensure the "generate_build_id_to_debug_symbols_mapping" task is run on this variant: {compile_variant if compile_variant else build_variant} in Evergreen to generate the stacktrace to symbolize.
Then, call "db-contrib-tool symbolize < my/stracktrace_json/file" in the command line.
This requires manual login authentication to Kanopy, but will then give a file that is symbolized after any needed debug symbols are downloaded.
To find the start of the symbolized stacktrace in terminal, look for the lines of this format: <source-file>:<line>:<column>: <(fully-qualified) function/symbol>.
There are various other optional parameters when symbolizing. For more clarification on these, use the command "db-contrib-tool symbolize --help" in your command line.
If no symbolized stacktrace is created, then most likely either:
    - None of the build ID's in the unsymbolized stacktrace are mapped to debug symbols, in which case double check the task run above is on the correct build / compile variant.
    -The unsymbolized stacktrace is not represented as a json or that json is not on just one line of the file, in which cause the parser will not be able to find a backtrace object.
    -You tried to symbolize a stacktrace that was deemed not symbolizable by its format.
            """

        with open(UNSYMBOLIZED_STACKTRACE_INSTRUCTIONS, "w") as outfile:
            outfile.write(content)

    def get_value_recursively(self, doc: Any, key: str) -> bool:
        """
        Search the dict recursively for a key.

        :param doc: Dict or any value to search in.
        :param key: Key to search for.
        :return: Value of the key or None.
        """
        try:
            if key in doc.keys():
                return True
            for sub_doc in doc.values():
                res = self.get_value_recursively(sub_doc, key)
                if res:
                    return True
        except AttributeError:
            pass
        return False

    def show_unsymbolizable_stacktraces(self, missing_keys: list[tuple[str, str]]):
        """
        Writes to the Evergreen user what files are not
        in proper formatting to be symbolized.

        :param output: Instructions to write to Evergreen
        :param missing_keys: Tuple(file_name, test_name) that are misformed
        """
        lines = [
            "The following file names, along with the test case they are associated with, cannot have their stacktrace symbolized due to missing keys that are needed for parsing"
        ]
        for file_name, test_name in missing_keys:
            lines.append(f"\t{file_name}, associated with the test {test_name}")
        return "\n".join(lines)

    def should_symbolize(self, test: TestCase) -> bool:
        """
        Check whether we should perform symbolization process.

        :param test: resmoke test case
        :return: whether we should symbolize
        """
        if self.config.skip_symbolization:
            test.logger.info("Configured to skip symbolization, skipping symbolization")
            return False

        if self.config.evg_task_id is None:
            test.logger.info("Not running in Evergreen, skipping symbolization")
            return False

        if self.config.client_id is None or self.config.client_secret is None:
            test.logger.info(
                "Symbolizer client secret and/or client ID are absent," " skipping symbolization"
            )
            return False

        if self.config.is_windows():
            test.logger.info("Running on Windows, skipping symbolization")
            return False

        if self.config.is_macos():
            test.logger.info("Running on MacOS, skipping symbolization")
            return False

        if HANG_ANALYZER_CALLED.is_set():
            test.logger.info(
                "Hang analyzer has been called, skipping symbolization to meet timeout constraints."
            )
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
        with _processed_files_lock:
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
        with _processed_files_lock:
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
            symbolizer_process = subprocess.Popen(
                args=symbolizer_args,
                close_fds=True,
                stdin=file_obj,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )

        try:
            output, _ = symbolizer_process.communicate(timeout=retry_timeout_secs)
        except subprocess.TimeoutExpired:
            symbolizer_process.kill()
            output, _ = symbolizer_process.communicate()

        return output.strip().decode()
