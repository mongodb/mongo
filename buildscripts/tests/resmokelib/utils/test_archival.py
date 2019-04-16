""" Unit tests for archival. """

import logging
import os
import random
import shutil
import tempfile
import unittest

from buildscripts.resmokelib.utils import archival

# pylint: disable=missing-docstring,protected-access

_BUCKET = "mongodatafiles"


def create_random_file(file_name, num_chars_mb):
    """ Creates file with random characters, which will have minimal compression. """
    with open(file_name, "wb") as fileh:
        for _ in range(num_chars_mb * 1024 * 1024):
            fileh.write(chr(random.randint(0, 255)))


class MockS3Client(object):
    """ Class to mock the S3 client. """

    def __init__(self, logger):
        self.logger = logger
        self.logger.info("MockS3Client init")

    def upload_file(self, *args, **kwargs):
        self.logger.info("MockS3Client upload_file %s %s", args, kwargs)

    def delete_object(self, *args, **kwargs):
        self.logger.info("MockS3Client delete_object %s %s", args, kwargs)


class ArchivalTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        logging.basicConfig()
        cls.logger = logging.getLogger()
        cls.logger.setLevel(logging.INFO)
        cls.bucket = _BUCKET
        cls.temp_dir = tempfile.mkdtemp()
        cls.remote_paths = []

        # We can use the AWS S3 calls by setting the environment variable MOCK_CLIENT to 0.
        mock_client = os.environ.get("MOCK_CLIENT", "1") != "0"
        if mock_client:
            cls.s3_client = MockS3Client(cls.logger)
        else:
            cls.s3_client = archival.Archival._get_s3_client()
        cls.archive = cls.create_archival()

    @classmethod
    def tearDownClass(cls):
        cls.archive.exit()
        cls.logger.info("Cleaning temp directory %s", cls.temp_dir)
        shutil.rmtree(cls.temp_dir, ignore_errors=True)
        for path in cls.remote_paths:
            cls.delete_s3_file(path)

    @classmethod
    def create_archival(cls):
        return archival.Archival(cls.logger, s3_client=cls.s3_client)

    @classmethod
    def delete_s3_file(cls, path):
        cls.logger.info("Cleaning S3 bucket %s path %s", cls.bucket, path)
        cls.s3_client.delete_object(Bucket=cls.bucket, Key=path)

    def s3_path(self, path, add_remote_path=True):
        if add_remote_path:
            self.remote_paths.append(path)
        return path


class ArchivalFileTests(ArchivalTestCase):
    def test_nofile(self):
        # Invalid input_files
        display_name = "Unittest invalid file"
        input_files = "no_file"
        s3_path = self.s3_path("unittest/no_file.tgz", False)
        self.assertRaises(
            OSError, lambda: self.archive.archive_files_to_s3(display_name, input_files, self.
                                                              bucket, s3_path))

        # Invalid input_files in a list
        input_files = ["no_file", "no_file2"]
        s3_path = self.s3_path("unittest/no_files.tgz", False)
        self.assertRaises(
            OSError, lambda: self.archive.archive_files_to_s3(display_name, input_files, self.
                                                              bucket, s3_path))

        # No files
        display_name = "Unittest no files"
        s3_path = self.s3_path("unittest/no_files.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, [], self.bucket, s3_path)
        self.assertEqual(1, status, message)

    def test_files(self):
        # Valid files
        display_name = "Unittest valid file"
        temp_file = tempfile.mkstemp(dir=self.temp_dir)[1]
        s3_path = self.s3_path("unittest/valid_file.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)

        # 2 valid files
        display_name = "Unittest 2 valid files"
        temp_file2 = tempfile.mkstemp(dir=self.temp_dir)[1]
        s3_path = self.s3_path("unittest/2valid_files.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, [temp_file, temp_file2],
                                                           self.bucket, s3_path)
        self.assertEqual(0, status, message)

    def test_empty_directory(self):
        # Valid directory
        display_name = "Unittest valid directory no files"
        temp_dir = tempfile.mkdtemp(dir=self.temp_dir)
        s3_path = self.s3_path("unittest/valid_directory.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, temp_dir, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)

        display_name = "Unittest valid directories no files"
        temp_dir2 = tempfile.mkdtemp(dir=self.temp_dir)
        s3_path = self.s3_path("unittest/valid_directories.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, [temp_dir, temp_dir2],
                                                           self.bucket, s3_path)
        self.assertEqual(0, status, message)

    def test_directory(self):
        display_name = "Unittest directory with files"
        temp_dir = tempfile.mkdtemp(dir=self.temp_dir)
        s3_path = self.s3_path("unittest/directory_with_files.tgz")
        # Create 10 empty files
        for _ in range(10):
            tempfile.mkstemp(dir=temp_dir)
        status, message = self.archive.archive_files_to_s3(display_name, temp_dir, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)

        display_name = "Unittest 2 valid directory files"
        temp_dir2 = tempfile.mkdtemp(dir=self.temp_dir)
        s3_path = self.s3_path("unittest/directories_with_files.tgz")
        # Create 10 empty files
        for _ in range(10):
            tempfile.mkstemp(dir=temp_dir2)
        status, message = self.archive.archive_files_to_s3(display_name, [temp_dir, temp_dir2],
                                                           self.bucket, s3_path)
        self.assertEqual(0, status, message)


class ArchivalLimitSizeTests(ArchivalTestCase):
    @classmethod
    def create_archival(cls):
        return archival.Archival(cls.logger, limit_size_mb=5, s3_client=cls.s3_client)

    def test_limit_size(self):

        # Files within limit size
        display_name = "Unittest under limit size"
        temp_file = tempfile.mkstemp(dir=self.temp_dir)[1]
        create_random_file(temp_file, 3)
        s3_path = self.s3_path("unittest/valid_limit_size.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)
        # Note the size limit is enforced after the file uploaded. Subsequent
        # uploads will not be permitted, once the limit has been reached.
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)

        # Files beyond limit size
        display_name = "Unittest over limit size"
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(1, status, message)


class ArchivalLimitFileTests(ArchivalTestCase):
    @classmethod
    def create_archival(cls):
        return archival.Archival(cls.logger, limit_files=3, s3_client=cls.s3_client)

    def test_limit_file(self):

        # Files within limit number
        display_name = "Unittest under limit number"
        temp_file = tempfile.mkstemp(dir=self.temp_dir)[1]
        s3_path = self.s3_path("unittest/valid_limit_number.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)

        # Files beyond limit number
        display_name = "Unittest over limit number"
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(1, status, message)


class ArchivalLimitTests(ArchivalTestCase):
    @classmethod
    def create_archival(cls):
        return archival.Archival(cls.logger, limit_size_mb=3, limit_files=3,
                                 s3_client=cls.s3_client)

    def test_limits(self):

        # Files within limits
        display_name = "Unittest under limits"
        temp_file = tempfile.mkstemp(dir=self.temp_dir)[1]
        create_random_file(temp_file, 1)
        s3_path = self.s3_path("unittest/valid_limits.tgz")
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(0, status, message)

        # Files beyond limits
        display_name = "Unittest over limits"
        status, message = self.archive.archive_files_to_s3(display_name, temp_file, self.bucket,
                                                           s3_path)
        self.assertEqual(1, status, message)
