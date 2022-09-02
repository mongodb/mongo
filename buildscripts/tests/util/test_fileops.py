"""Unit tests for fileops.py."""
import unittest
from unittest.mock import patch

import buildscripts.util.fileops as under_test


class TestWriteFileToDir(unittest.TestCase):
    @patch("os.path.exists")
    @patch("buildscripts.util.fileops.write_file")
    def test_existing_file_can_be_overriden(self, mock_write_file, mock_exists):
        mock_exists.return_value = True

        under_test.write_file_to_dir("dir", "file", "contents", overwrite=True)

        mock_write_file.assert_called()

    @patch("os.path.exists")
    @patch("buildscripts.util.fileops.write_file")
    def test_existing_file_can_cause_exception(self, mock_write_file, mock_exists):
        mock_exists.return_value = True

        with self.assertRaises(FileExistsError):
            under_test.write_file_to_dir("dir", "file", "contents", overwrite=False)

        mock_write_file.assert_not_called()
