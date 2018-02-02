"""
Archival utility.
"""

from __future__ import absolute_import

import Queue
import collections
import json
import math
import os
import tarfile
import tempfile
import threading
import time

UploadArgs = collections.namedtuple(
    "UploadArgs",
    ["archival_file",
     "display_name",
     "local_file",
     "content_type",
     "s3_bucket",
     "s3_path",
     "delete_file"])

ArchiveArgs = collections.namedtuple(
    "ArchiveArgs", ["archival_file", "display_name", "remote_file"])


def file_list_size(files):
    """ Return size (in bytes) of all 'files' and their subdirectories. """
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
    """ Return size (in bytes) of files in 'directory' tree. """
    dir_bytes = 0
    for root_dir, _, files in os.walk(directory):
        for name in files:
            full_name = os.path.join(root_dir, name)
            try:
                dir_bytes += os.path.getsize(full_name)
            except OSError:
                # Symlinks generate an error and are ignored.
                if os.path.islink(full_name):
                    pass
                else:
                    raise
    return dir_bytes


def free_space(path):
    """ Return file system free space (in bytes) for 'path'. """
    stat = os.statvfs(path)
    return stat.f_bavail * stat.f_bsize


class Archival(object):
    """ Class to support file archival to S3."""

    def __init__(self,
                 logger,
                 archival_json_file="archive.json",
                 execution=0,
                 limit_size_mb=0,
                 limit_files=0,
                 s3_client=None):
        """ Archival init method. """

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
        self._archive_file_queue = Queue.Queue()
        self._archive_file_worker = threading.Thread(
            target=self._update_archive_file_wkr,
            args=(self._archive_file_queue, execution, logger),
            name="archive_file_worker")
        self._archive_file_worker.setDaemon(True)
        self._archive_file_worker.start()
        if not s3_client:
            self.s3_client = self._get_s3_client()
        else:
            self.s3_client = s3_client

        # Start the worker thread which uploads the archive.
        self._upload_queue = Queue.Queue()
        self._upload_worker = threading.Thread(
            target=self._upload_to_s3_wkr,
            args=(self._upload_queue, self._archive_file_queue, logger, self.s3_client),
            name="upload_worker")
        self._upload_worker.setDaemon(True)
        self._upload_worker.start()

    @staticmethod
    def _get_s3_client():
        # Since boto3 is a 3rd party module, we import locally.
        import boto3
        return boto3.client("s3")

    def archive_files_to_s3(self, display_name, input_files, s3_bucket, s3_path):
        """
        Archive 'input_files' to 's3_bucket' and 's3_path'.

        Archive is not done if user specified limits are reached. The size limit is
        enforced after it has been exceeded, since it can only be calculated after the
        tar/gzip has been done.

        Returns status and message, where message contains information if status is non-0.
        """

        start_time = time.time()
        with self._lock:
            if not input_files:
                status = 1
                message = "No input_files specified"
            elif self.limit_size_mb and self.size_mb >= self.limit_size_mb:
                status = 1
                message = "Files not archived, limit size {}MB reached".format(self.limit_size_mb)
            elif self.limit_files and self.num_files >= self.limit_files:
                status = 1
                message = "Files not archived, limit files {} reached".format(self.limit_files)
            else:
                status, message, file_size_mb = self._archive_files(
                    display_name,
                    input_files,
                    s3_bucket,
                    s3_path)

                self.num_files += 1
                self.size_mb += file_size_mb
                self.archive_time += time.time() - start_time

        return status, message

    @staticmethod
    def _update_archive_file_wkr(queue, execution, logger):
        """ Worker thread: Update the archival JSON file from 'queue'. """
        archival_json = {"files": [], "execution": execution}
        while True:
            archive_args = queue.get()
            # Exit worker thread when sentinel is received.
            if archive_args is None:
                queue.task_done()
                break
            logger.debug("Updating archive file %s", archive_args.archival_file)
            archival_record = {
                "name": archive_args.display_name,
                "link": archive_args.remote_file,
                "visibility": "private"
            }
            archival_json["files"].append(archival_record)
            with open(archive_args.archival_file, "w") as archival_fh:
                json.dump([archival_json], archival_fh)
            queue.task_done()

    @staticmethod
    def _upload_to_s3_wkr(queue, archive_file_queue, logger, s3_client):
        """" Worker thread: Upload to S3 from 'queue', dispatch to 'archive_file_queue'. """
        while True:
            upload_args = queue.get()
            # Exit worker thread when sentinel is received.
            if upload_args is None:
                queue.task_done()
                archive_file_queue.put(None)
                break
            extra_args = {"ContentType": upload_args.content_type, "ACL": "public-read"}
            logger.debug("Uploading to S3 %s to %s %s",
                         upload_args.local_file,
                         upload_args.s3_bucket,
                         upload_args.s3_path)
            upload_completed = False
            try:
                s3_client.upload_file(upload_args.local_file,
                                      upload_args.s3_bucket,
                                      upload_args.s3_path,
                                      ExtraArgs=extra_args)
                upload_completed = True
                logger.debug("Upload to S3 completed for %s to bucket %s path %s",
                             upload_args.local_file,
                             upload_args.s3_bucket,
                             upload_args.s3_path)
            except Exception as err:
                logger.exception("Upload to S3 error %s", err)

            if upload_args.delete_file:
                os.remove(upload_args.local_file)

            remote_file = "https://s3.amazonaws.com/{}/{}".format(
                upload_args.s3_bucket, upload_args.s3_path)
            if upload_completed:
                archive_file_queue.put(ArchiveArgs(
                    upload_args.archival_file, upload_args.display_name, remote_file))

            queue.task_done()

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

        message = "Tar/gzip {} files: {}".format(display_name, input_files)
        status = 0
        size_mb = 0

        # Tar/gzip to a temporary file.
        temp_file = tempfile.NamedTemporaryFile(suffix=".tgz", delete=False)
        local_file = temp_file.name

        # Check if there is sufficient space for the temporary tgz file.
        if file_list_size(input_files) > free_space(local_file):
            os.remove(local_file)
            return 1, "Insufficient space for {}".format(message), 0

        try:
            with tarfile.open(local_file, "w:gz") as tar_handle:
                for input_file in input_files:
                    tar_handle.add(input_file)
        except (IOError, tarfile.TarError) as err:
            message = str(err)
            status = 1

        # Round up the size of archive.
        size_mb = int(math.ceil(float(file_list_size(local_file)) / (1024 * 1024)))
        self._upload_queue.put(UploadArgs(
            self.archival_json_file,
            display_name,
            local_file,
            "application/x-gzip",
            s3_bucket,
            s3_path,
            True))

        return status, message, size_mb

    def check_thread(self, thread, expected_alive):
        if thread.isAlive() and not expected_alive:
            self.logger.warning(
                "The %s thread did not complete, some files might not have been uploaded"
                " to S3 or archived to %s.", thread.name, self.archival_json_file)
        elif not thread.isAlive() and expected_alive:
            self.logger.warning(
                "The %s thread is no longer running, some files might not have been uploaded"
                " to S3 or archived to %s.", thread.name, self.archival_json_file)

    def exit(self, timeout=30):
        """ Waits for worker threads to finish. """
        # Put sentinel on upload queue to trigger worker thread exit.
        self._upload_queue.put(None)
        self.check_thread(self._upload_worker, True)
        self.check_thread(self._archive_file_worker, True)
        self._upload_worker.join(timeout=timeout)
        self.check_thread(self._upload_worker, False)

        # Archive file worker thread exit should be triggered by upload thread worker.
        self._archive_file_worker.join(timeout=timeout)
        self.check_thread(self._archive_file_worker, False)

        self.logger.info("Total tar/gzip archive time is %0.2f seconds", self.archive_time)

    def files_archived_num(self):
        """ Returns the number of the archived files. """
        return self.num_files

    def files_archived_size_mb(self):
        """ Returns the size of the archived files. """
        return self.size_mb
