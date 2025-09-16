"""Archival utility."""

import collections
import json
import math
import os
import queue
import sys
import tarfile
import tempfile
import threading
import time
from abc import ABC, abstractmethod
from pathlib import Path
from typing import List

from buildscripts.resmokelib import config

_IS_WINDOWS = sys.platform in ("win32", "cygwin")

if _IS_WINDOWS:
    import ctypes

UploadArgs = collections.namedtuple(
    "UploadArgs",
    [
        "archival_file",
        "display_name",
        "local_file",
        "content_type",
        "s3_bucket",
        "s3_path",
        "delete_file",
    ],
)

ArchiveArgs = collections.namedtuple(
    "ArchiveArgs", ["archival_file", "display_name", "remote_file"]
)


def file_list_size(files):
    """Return size (in bytes) of all 'files' and their subdirectories."""
    if isinstance(files, str):
        files = [files]
    file_bytes = 0
    for ifile in files:
        if not os.path.exists(ifile):
            pass
        elif os.path.isdir(ifile):
            file_bytes += directory_size(ifile)
        else:
            file_bytes += os.path.getsize(ifile)
    return file_bytes


def directory_size(directory):
    """Return size (in bytes) of files in 'directory' tree."""
    dir_bytes = 0
    for root_dir, _, files in os.walk(str(directory)):
        for name in files:
            full_name = os.path.join(root_dir, name)
            try:
                dir_bytes += os.path.getsize(full_name)
            except OSError:
                # A file might be deleted while we are looping through the os.walk() result.
                pass
    return dir_bytes


def free_space(path):
    """Return file system free space (in bytes) for 'path'."""
    if _IS_WINDOWS:
        dirname = os.path.dirname(path)
        free_bytes = ctypes.c_ulonglong(0)
        ctypes.windll.kernel32.GetDiskFreeSpaceExW(
            ctypes.c_wchar_p(dirname), None, None, ctypes.pointer(free_bytes)
        )
        return free_bytes.value
    stat = os.statvfs(path)
    return stat.f_bavail * stat.f_bsize


def remove_file(file_name):
    """Attempt to remove file. Return status and message."""
    try:
        # File descriptors, on Windows, are inherited by all subprocesses and file removal may fail
        # because the file is still open.
        # See https://www.python.org/dev/peps/pep-0446/#issues-with-inheritable-file-descriptors
        os.remove(file_name)
        status = 0
        message = "Successfully deleted file {}".format(file_name)
    except Exception as err:
        status = 1
        message = "Error deleting file {}: {}".format(file_name, err)
    return status, message


def get_archive_path(path: str) -> str:
    """Shortens paths that are relative to DBPATH_PREFIX"""
    if config.DBPATH_PREFIX:
        base = Path(config.DBPATH_PREFIX)
        if Path(path).is_relative_to(base):
            return Path(path).relative_to(base)
    return path


class ArchiveStrategy(ABC):
    @abstractmethod
    def archive_files(self, files: List[str], archive_name: str, display_name: str):
        pass

    def exit(self):
        pass

    def create_tar_archive(self, files: List[str], archive: Path, display_name: str):
        status = 0
        size_mb = 0
        message = "Tar/gzip {} files: {}".format(display_name, files)

        # Check if there is sufficient space for the tgz file.
        if file_list_size(files) > free_space(archive):
            status, message = remove_file(archive)
            if status:
                self.logger.warning("Removing tarfile due to insufficient space - %s", message)
            return 1, "Insufficient space for {}".format(message), 0

        try:
            with tarfile.open(archive, "w:gz") as tar_handle:
                for input_file in files:
                    try:
                        tar_handle.add(input_file, get_archive_path(input_file))
                    except (IOError, OSError, tarfile.TarError) as err:
                        message = "{}; Unable to add {} to archive file: {}".format(
                            message, input_file, err
                        )
        except (IOError, OSError, tarfile.TarError) as err:
            status, message = remove_file(archive)
            if status:
                self.logger.warning("Removing tarfile due to creation failure - %s", message)
            return 1, str(err), 0

        # Round up the size of the archive.
        size_mb = int(math.ceil(float(file_list_size(archive)) / (1024 * 1024)))

        return status, message, size_mb


class ArchiveToS3(ArchiveStrategy):
    def __init__(
        self,
        bucket: str,
        s3_base_path: str,
        client,
        logger,
        archival_json_file="archive.json",
    ):
        self.bucket = bucket
        self.s3_base_path = s3_base_path
        self.s3_client = client
        self.logger = logger
        self.archival_json_file = archival_json_file

        # Start the worker thread to update the 'archival_json_file'.
        self._archive_file_queue = queue.Queue()
        self._archive_file_worker = threading.Thread(
            target=self._update_archive_file_wkr,
            args=(self._archive_file_queue, logger),
            name="archive_file_worker",
        )
        self._archive_file_worker.daemon = True
        self._archive_file_worker.start()

        # Start the worker thread which uploads the archive.
        self._upload_queue = queue.Queue()
        self._upload_worker = threading.Thread(
            target=self._upload_to_s3_wkr,
            args=(self._upload_queue, self._archive_file_queue, logger, self.s3_client),
            name="upload_worker",
        )
        self._upload_worker.daemon = True
        self._upload_worker.start()

    @staticmethod
    def _update_archive_file_wkr(work_queue, logger):
        """Worker thread: Update the archival JSON file from 'work_queue'."""
        archival_json = []
        while True:
            archive_args = work_queue.get()
            # Exit worker thread when sentinel is received.
            if archive_args is None:
                work_queue.task_done()
                break
            archival_record = {
                "name": archive_args.display_name,
                "link": archive_args.remote_file,
                "visibility": "private",
            }
            logger.debug(
                "Updating archive file %s with %s", archive_args.archival_file, archival_record
            )
            archival_json.append(archival_record)
            with open(archive_args.archival_file, "w") as archival_fh:
                json.dump(archival_json, archival_fh)
            work_queue.task_done()

    @staticmethod
    def _upload_to_s3_wkr(work_queue, archive_file_work_queue, logger, s3_client):
        """Worker thread: Upload to S3 from 'work_queue', dispatch to 'archive_file_work_queue'."""
        while True:
            upload_args = work_queue.get()
            # Exit worker thread when sentinel is received.
            if upload_args is None:
                work_queue.task_done()
                archive_file_work_queue.put(None)
                break
            extra_args = {"ContentType": upload_args.content_type, "ACL": "public-read"}
            logger.debug(
                "Uploading to S3 %s to bucket %s path %s",
                upload_args.local_file,
                upload_args.s3_bucket,
                upload_args.s3_path,
            )
            upload_completed = False
            try:
                s3_client.upload_file(
                    upload_args.local_file,
                    upload_args.s3_bucket,
                    upload_args.s3_path,
                    ExtraArgs=extra_args,
                )
                upload_completed = True
                logger.debug(
                    "Upload to S3 completed for %s to bucket %s path %s",
                    upload_args.local_file,
                    upload_args.s3_bucket,
                    upload_args.s3_path,
                )
            except Exception as err:
                logger.exception("Upload to S3 error %s", err)

            if upload_args.delete_file:
                status, message = remove_file(upload_args.local_file)
                if status:
                    logger.error("Upload to S3 delete file error %s", message)

            remote_file = "https://s3.amazonaws.com/{}/{}".format(
                upload_args.s3_bucket, upload_args.s3_path
            )
            if upload_completed:
                archive_file_work_queue.put(
                    ArchiveArgs(upload_args.archival_file, upload_args.display_name, remote_file)
                )

            work_queue.task_done()

    def check_thread(self, thread, expected_alive):
        """Check if the thread is still active."""
        if thread.is_alive() and not expected_alive:
            self.logger.warning(
                "The %s thread did not complete, some files might not have been uploaded"
                " to S3 or archived to %s.",
                thread.name,
                self.archival_json_file,
            )
        elif not thread.is_alive() and expected_alive:
            self.logger.warning(
                "The %s thread is no longer running, some files might not have been uploaded"
                " to S3 or archived to %s.",
                thread.name,
                self.archival_json_file,
            )

    def exit(self, timeout=30):
        """Wait for worker threads to finish."""
        # Put sentinel on upload queue to trigger worker thread exit.
        self._upload_queue.put(None)
        self.check_thread(self._upload_worker, True)
        self.check_thread(self._archive_file_worker, True)
        self._upload_worker.join(timeout=timeout)
        self.check_thread(self._upload_worker, False)

        # Archive file worker thread exit should be triggered by upload thread worker.
        self._archive_file_worker.join(timeout=timeout)
        self.check_thread(self._archive_file_worker, False)

    def archive_files(self, files: List[str], archive_name: str, display_name: str):
        _, temp_file = tempfile.mkstemp(suffix=".tgz")
        status, message, size_mb = self.create_tar_archive(files, temp_file, display_name)
        self._upload_queue.put(
            UploadArgs(
                self.archival_json_file,
                display_name,
                temp_file,
                "application/x-gzip",
                self.bucket,
                f"{self.s3_base_path}{archive_name}",
                True,
            )
        )
        return status, message, size_mb


class ArchiveToDirectory(ArchiveStrategy):
    def __init__(self, directory: Path, logger):
        self.directory = directory
        self.logger = logger
        os.makedirs(self.directory, exist_ok=True)

    def archive_files(self, files: List[str], archive_name: str, display_name: str):
        archive = os.path.join(self.directory, archive_name)
        with open(archive, "w") as _:
            pass  # Touch create the archive file, so we can check the size of the disk it is on before adding to it.
        return self.create_tar_archive(files, archive, display_name)


class TestArchival(ArchiveStrategy):
    def __init__(self):
        self.archive_file = os.path.join(config.DBPATH_PREFIX, "test_archival.txt")

    def archive_files(self, files: List[str], archive_name: str, display_name: str):
        with open(self.archive_file, "a") as f:
            for file in files:
                # If a resmoke fixture is used, the input_file will be the source of the data
                # files. If mongorunner is used, input_file/mongorunner will be the source
                # of the data files.
                if os.path.isdir(os.path.join(file, config.MONGO_RUNNER_SUBDIR)):
                    file = os.path.join(file, config.MONGO_RUNNER_SUBDIR)

                # Each node contains one directory for its data files. Here we write out
                # the names of those directories. In the unit test for archival, we will
                # check that the directories are those we expect.
                f.write("\n".join(os.listdir(file)) + "\n")
        message = "'test_archival' specified. Skipping tar/gzip."
        return 0, message, 0


class Archival(object):
    """Class to support file archival to S3."""

    def __init__(
        self,
        archive_strategy: ArchiveStrategy,
        logger,
        archival_json_file="archive.json",
        limit_size_mb=0,
        limit_files=0,
        s3_client=None,
    ):
        """Initialize Archival."""

        self.archival_json_file = archival_json_file
        self.limit_size_mb = limit_size_mb
        self.limit_files = limit_files
        self.size_mb = 0
        self.num_files = 0
        self.archive_time = 0
        self.logger = logger
        self.archive_strategy = archive_strategy

        # Lock to control access from multiple threads.
        self._lock = threading.Lock()

    def archive_files(self, input_files: str | List[str], archive_name: str, display_name: str):
        if isinstance(input_files, str):
            input_files = [input_files]

        start_time = time.time()
        with self._lock:
            if not input_files:
                status = 1
                message = "No input_files specified"
            elif self.limit_size_mb and self.size_mb >= self.limit_size_mb:
                status = 1
                message = "Files not archived, {}MB size limit reached".format(self.limit_size_mb)
            elif self.limit_files and self.num_files >= self.limit_files:
                status = 1
                message = "Files not archived, {} file limit reached".format(self.limit_files)
            else:
                status, message, file_size_mb = self.archive_strategy.archive_files(
                    input_files, archive_name, display_name
                )
                if status == 0:
                    self.num_files += 1
                self.size_mb += file_size_mb
                self.archive_time += time.time() - start_time

        return status, message

    def exit(self):
        self.archive_strategy.exit()
        self.logger.info(
            "Total tar/gzip archive time is %0.2f seconds, for %d file(s) %d MB",
            self.archive_time,
            self.num_files,
            self.size_mb,
        )
