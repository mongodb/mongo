"""Archival utility."""

import collections
import json
import os
import queue
import sys
import tarfile
import tempfile
import threading
import time

import math

from buildscripts.resmokelib import config

_IS_WINDOWS = sys.platform == "win32" or sys.platform == "cygwin"

if _IS_WINDOWS:
    import ctypes

UploadArgs = collections.namedtuple("UploadArgs", [
    "archival_file", "display_name", "local_file", "content_type", "s3_bucket", "s3_path",
    "delete_file"
])

ArchiveArgs = collections.namedtuple("ArchiveArgs",
                                     ["archival_file", "display_name", "remote_file"])


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
            ctypes.c_wchar_p(dirname), None, None, ctypes.pointer(free_bytes))
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
    except Exception as err:  # pylint: disable=broad-except
        status = 1
        message = "Error deleting file {}: {}".format(file_name, err)
    return status, message


class Archival(object):
    """Class to support file archival to S3."""

    def __init__(self, logger, archival_json_file="archive.json", limit_size_mb=0, limit_files=0,
                 s3_client=None):
        """Initialize Archival."""

        self.archival_json_file = archival_json_file
        self.limit_size_mb = limit_size_mb
        self.limit_files = limit_files
        self.size_mb = 0
        self.num_files = 0
        self.archive_time = 0
        self.logger = logger

        # Lock to control access from multiple threads.
        self._lock = threading.Lock()

        # Start the worker thread to update the 'archival_json_file'.
        self._archive_file_queue = queue.Queue()
        self._archive_file_worker = threading.Thread(target=self._update_archive_file_wkr,
                                                     args=(self._archive_file_queue,
                                                           logger), name="archive_file_worker")
        self._archive_file_worker.setDaemon(True)
        self._archive_file_worker.start()
        if not s3_client:
            self.s3_client = self._get_s3_client()
        else:
            self.s3_client = s3_client

        # Start the worker thread which uploads the archive.
        self._upload_queue = queue.Queue()
        self._upload_worker = threading.Thread(
            target=self._upload_to_s3_wkr, args=(self._upload_queue, self._archive_file_queue,
                                                 logger, self.s3_client), name="upload_worker")
        self._upload_worker.setDaemon(True)
        self._upload_worker.start()

    @staticmethod
    def _get_s3_client():
        # Since boto3 is a 3rd party module, we import locally.
        import boto3
        return boto3.client("s3")

    def archive_files_to_s3(self, display_name, input_files, s3_bucket, s3_path):
        """Archive 'input_files' to 's3_bucket' and 's3_path'.

        Archive is not done if user specified limits are reached. The size limit is
        enforced after it has been exceeded, since it can only be calculated after the
        tar/gzip has been done.

        Return status and message, where message contains information if status is non-0.
        """

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
                status, message, file_size_mb = self._archive_files(display_name, input_files,
                                                                    s3_bucket, s3_path)

                if status == 0:
                    self.num_files += 1
                self.size_mb += file_size_mb
                self.archive_time += time.time() - start_time

        return status, message

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
                "name": archive_args.display_name, "link": archive_args.remote_file,
                "visibility": "private"
            }
            logger.debug("Updating archive file %s with %s", archive_args.archival_file,
                         archival_record)
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
            logger.debug("Uploading to S3 %s to bucket %s path %s", upload_args.local_file,
                         upload_args.s3_bucket, upload_args.s3_path)
            upload_completed = False
            try:
                s3_client.upload_file(upload_args.local_file, upload_args.s3_bucket,
                                      upload_args.s3_path, ExtraArgs=extra_args)
                upload_completed = True
                logger.debug("Upload to S3 completed for %s to bucket %s path %s",
                             upload_args.local_file, upload_args.s3_bucket, upload_args.s3_path)
            except Exception as err:  # pylint: disable=broad-except
                logger.exception("Upload to S3 error %s", err)

            if upload_args.delete_file:
                status, message = remove_file(upload_args.local_file)
                if status:
                    logger.error("Upload to S3 delete file error %s", message)

            remote_file = "https://s3.amazonaws.com/{}/{}".format(upload_args.s3_bucket,
                                                                  upload_args.s3_path)
            if upload_completed:
                archive_file_work_queue.put(
                    ArchiveArgs(upload_args.archival_file, upload_args.display_name, remote_file))

            work_queue.task_done()

    def _archive_files(self, display_name, input_files, s3_bucket, s3_path):
        """
        Gather 'input_files' into a single tar/gzip and archive to 's3_path'.

        The caller waits until the list of files has been tar/gzipped to a temporary file.
        The S3 upload and subsequent update to 'archival_json_file' will be done asynchronosly.

        Returns status, message and size_mb of archive.
        """

        # Parameter 'input_files' can either be a string or list of strings.
        if isinstance(input_files, str):
            input_files = [input_files]

        status = 0
        size_mb = 0

        if 'test_archival' in config.INTERNAL_PARAMS:
            message = "'test_archival' specified. Skipping tar/gzip."
            with open(os.path.join(config.DBPATH_PREFIX, "test_archival.txt"), "a") as test_file:
                for input_file in input_files:
                    # If a resmoke fixture is used, the input_file will be the source of the data
                    # files. If mongorunner is used, input_file/mongorunner will be the source
                    # of the data files.
                    if os.path.isdir(os.path.join(input_file, config.MONGO_RUNNER_SUBDIR)):
                        input_file = os.path.join(input_file, config.MONGO_RUNNER_SUBDIR)

                    # Each node contains one directory for its data files. Here we write out
                    # the names of those directories. In the unit test for archival, we will
                    # check that the directories are those we expect.
                    test_file.write("\n".join(os.listdir(input_file)) + "\n")
            return status, message, size_mb

        message = "Tar/gzip {} files: {}".format(display_name, input_files)

        # Tar/gzip to a temporary file.
        _, temp_file = tempfile.mkstemp(suffix=".tgz")

        # Check if there is sufficient space for the temporary tgz file.
        if file_list_size(input_files) > free_space(temp_file):
            status, message = remove_file(temp_file)
            if status:
                self.logger.warning("Removing tarfile due to insufficient space - %s", message)
            return 1, "Insufficient space for {}".format(message), 0

        try:
            with tarfile.open(temp_file, "w:gz") as tar_handle:
                for input_file in input_files:
                    try:
                        tar_handle.add(input_file)
                    except (IOError, OSError, tarfile.TarError) as err:
                        message = "{}; Unable to add {} to archive file: {}".format(
                            message, input_file, err)
        except (IOError, OSError, tarfile.TarError) as err:
            status, message = remove_file(temp_file)
            if status:
                self.logger.warning("Removing tarfile due to creation failure - %s", message)
            return 1, str(err), 0

        # Round up the size of the archive.
        size_mb = int(math.ceil(float(file_list_size(temp_file)) / (1024 * 1024)))  # pylint: disable=c-extension-no-member
        self._upload_queue.put(
            UploadArgs(self.archival_json_file, display_name, temp_file, "application/x-gzip",
                       s3_bucket, s3_path, True))

        return status, message, size_mb

    def check_thread(self, thread, expected_alive):
        """Check if the thread is still active."""
        if thread.is_alive() and not expected_alive:
            self.logger.warning(
                "The %s thread did not complete, some files might not have been uploaded"
                " to S3 or archived to %s.", thread.name, self.archival_json_file)
        elif not thread.is_alive() and expected_alive:
            self.logger.warning(
                "The %s thread is no longer running, some files might not have been uploaded"
                " to S3 or archived to %s.", thread.name, self.archival_json_file)

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

        self.logger.info("Total tar/gzip archive time is %0.2f seconds, for %d file(s) %d MB",
                         self.archive_time, self.num_files, self.size_mb)

    def files_archived_num(self):
        """Return the number of the archived files."""
        return self.num_files

    def files_archived_size_mb(self):
        """Return the size of the archived files."""
        return self.size_mb
