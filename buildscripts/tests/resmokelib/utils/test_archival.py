"""Unit tests for archival."""

import logging
import os
import shutil
import tempfile
import unittest
from unittest.mock import ANY, MagicMock

from buildscripts.resmokelib.utils import archival

_BUCKET = "mongodatafiles"


def create_random_file(file_name, num_chars_mb):
    """Creates file with random characters, which will have minimal compression."""
    with open(file_name, "wb") as fileh:
        fileh.write(os.urandom(num_chars_mb * 1024 * 1024))


class S3ArchivalTestCase(unittest.TestCase):
    def setUp(self):
        logging.basicConfig()
        self.logger = logging.getLogger()
        self.logger.setLevel(logging.INFO)
        self.bucket = _BUCKET
        self.temp_dir = tempfile.mkdtemp()
        self.remote_paths = []
        self.s3_client = MagicMock()
        self.base_path = "project/variant/revision/datafiles/taskid-"
        self.archive = self.create_archival()

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def create_archival(self):
        return archival.Archival(
            archival.ArchiveToS3(self.bucket, self.base_path, self.s3_client, self.logger),
            self.logger,
            s3_client=self.s3_client,
        )


class ArchivalFileTests(S3ArchivalTestCase):
    def test_file(self):
        # Archive a single data file
        data_files = tempfile.mkstemp(dir=self.temp_dir)[1]
        archive_name = "archive.tgz"
        display_name = "Single-file archive"

        status, message = self.archive.archive_files(
            data_files,
            archive_name,
            display_name,
        )
        self.archive.exit()

        self.assertEqual(0, status, message)
        self.s3_client.upload_file.assert_called_with(
            ANY, self.bucket, f"{self.base_path}{archive_name}", ExtraArgs=ANY
        )

    def test_files(self):
        # Archive multiple data files in one upload
        data_files = [
            tempfile.mkstemp(dir=self.temp_dir)[1],
            tempfile.mkstemp(dir=self.temp_dir)[1],
        ]
        archive_name = "archive.tgz"
        display_name = "Multi-file archive"

        status, message = self.archive.archive_files(
            data_files,
            archive_name,
            display_name,
        )
        self.archive.exit()

        self.assertEqual(0, status, message)
        self.s3_client.upload_file.assert_called_with(
            ANY, self.bucket, f"{self.base_path}{archive_name}", ExtraArgs=ANY
        )

    def test_empty_directory(self):
        data_files = tempfile.mkdtemp(dir=self.temp_dir)
        archive_name = "archive.tgz"
        display_name = "Archive"

        status, message = self.archive.archive_files(
            data_files,
            archive_name,
            display_name,
        )
        self.archive.exit()

        self.assertEqual(0, status, message)
        self.s3_client.upload_file.assert_called_with(
            ANY, self.bucket, f"{self.base_path}{archive_name}", ExtraArgs=ANY
        )

    def test_empty_directories(self):
        data_files = [tempfile.mkdtemp(dir=self.temp_dir), tempfile.mkdtemp(dir=self.temp_dir)]
        archive_name = "archive.tgz"
        display_name = "Archive"

        status, message = self.archive.archive_files(
            data_files,
            archive_name,
            display_name,
        )
        self.archive.exit()

        self.assertEqual(0, status, message)
        self.s3_client.upload_file.assert_called_with(
            ANY, self.bucket, f"{self.base_path}{archive_name}", ExtraArgs=ANY
        )

    def test_directory(self):
        data_files = tempfile.mkdtemp(dir=self.temp_dir)
        for _ in range(10):
            tempfile.mkstemp(dir=data_files)
        archive_name = "archive.tgz"
        display_name = "Archive"

        status, message = self.archive.archive_files(
            data_files,
            archive_name,
            display_name,
        )
        self.archive.exit()

        self.assertEqual(0, status, message)
        self.s3_client.upload_file.assert_called_with(
            ANY, self.bucket, f"{self.base_path}{archive_name}", ExtraArgs=ANY
        )

    def test_directories(self):
        data_files = []
        for _ in range(2):
            dir = tempfile.mkdtemp(dir=self.temp_dir)
            data_files.append(dir)
            for _ in range(10):
                tempfile.mkstemp(dir=dir)
        archive_name = "archive.tgz"
        display_name = "Archive"

        status, message = self.archive.archive_files(
            data_files,
            archive_name,
            display_name,
        )
        self.archive.exit()

        self.assertEqual(0, status, message)
        self.s3_client.upload_file.assert_called_with(
            ANY, self.bucket, f"{self.base_path}{archive_name}", ExtraArgs=ANY
        )


class ArchivalLimitSizeTests(S3ArchivalTestCase):
    def create_archival(self):
        return archival.Archival(
            archival.ArchiveToS3(self.bucket, self.base_path, self.s3_client, self.logger),
            self.logger,
            limit_size_mb=3,
            s3_client=self.s3_client,
        )

    def test_limit_size(self):
        # Files within limit size
        data_files = tempfile.mkstemp(dir=self.temp_dir)[1]
        create_random_file(data_files, 2)
        archive_name = "archive.tgz"
        display_name = "Archive"

        # First should upload successfully, the second should not result in an s3 upload_file call.
        for i in range(2):
            archive_name = f"archive_{i}.tgz"
            display_name = f"Archive {i}"
            status, message = self.archive.archive_files(
                data_files,
                archive_name,
                display_name,
            )
        self.archive.exit()

        self.assertEqual(self.s3_client.upload_file.call_count, 1)
        self.assertEqual(1, status, message)


class ArchivalLimitFileTests(S3ArchivalTestCase):
    def create_archival(self):
        return archival.Archival(
            archival.ArchiveToS3(self.bucket, self.base_path, self.s3_client, self.logger),
            self.logger,
            limit_files=3,
            s3_client=self.s3_client,
        )

    def test_limit_file(self):
        data_files = tempfile.mkstemp(dir=self.temp_dir)[1]

        # First 3 should upload successfully, the fourth should not result in an s3 upload_file call.
        for i in range(4):
            archive_name = f"archive_{i}.tgz"
            display_name = f"Archive {i}"
            status, message = self.archive.archive_files(
                data_files,
                archive_name,
                display_name,
            )

        self.archive.exit()
        self.assertEqual(1, status, message)
        self.assertEqual(self.s3_client.upload_file.call_count, 3)


class DirectoryArchivalTests(unittest.TestCase):
    def setUp(self):
        logging.basicConfig()
        self.logger = logging.getLogger()
        self.logger.setLevel(logging.INFO)
        self.temp_dir = tempfile.mkdtemp()
        self.s3_client = MagicMock()
        self.archive = archival.Archival(
            archival.ArchiveToDirectory(self.temp_dir, self.logger),
            self.logger,
        )

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_file(self):
        data_files = tempfile.mkstemp(dir=self.temp_dir)[1]
        archive_name = "archive.tgz"
        display_name = "Single-file archive"

        status, message = self.archive.archive_files(
            data_files,
            archive_name,
            display_name,
        )
        self.archive.exit()

        expected = os.path.join(self.temp_dir, archive_name)
        self.assertTrue(os.path.exists(expected), f"Expected archive does not exist: {expected}")
        self.assertEqual(0, status, message)
