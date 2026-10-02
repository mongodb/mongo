"""Unit tests for the simulate-crash hook."""

import os
import shutil
import tempfile
import unittest
import unittest.mock

from buildscripts.resmokelib.testing.hooks import simulate_crash


class TestCopyFile(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.new_root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.addCleanup(shutil.rmtree, self.new_root, ignore_errors=True)
        self.hook = simulate_crash.SimulateCrash.__new__(simulate_crash.SimulateCrash)
        self.hook.logger = unittest.mock.Mock()

    def _make_file(self, relative_path, contents):
        absolute_path = os.path.join(self.root, relative_path)
        os.makedirs(os.path.dirname(absolute_path), exist_ok=True)
        with open(absolute_path, "wb") as f:
            f.write(contents)
        # copy_file() assumes the destination dir already exists (capture_db's job).
        os.makedirs(os.path.dirname(os.path.join(self.new_root, relative_path)), exist_ok=True)
        return absolute_path

    def _copy_file(self, absolute_filepath):
        self.hook.copy_file(self.root, absolute_filepath, self.new_root)

    def test_copies_full_file(self):
        contents = b"hello world" * 1000
        absolute_filepath = self._make_file("data/file.wt", contents)

        self._copy_file(absolute_filepath)

        with open(os.path.join(self.new_root, "data/file.wt"), "rb") as f:
            self.assertEqual(f.read(), contents)
        self.hook.logger.warning.assert_not_called()

    def test_truncates_gracefully_when_file_shrinks_mid_copy(self):
        contents = b"x" * 1000
        absolute_filepath = self._make_file("data/file.wt", contents)

        # Truncate the file right after it's stat'd (reporting the original size) but
        # before the copy finishes, so the read loop hits a real short read/EOF.
        real_fstat = os.fstat
        call_count = 0

        def flaky_fstat(fd):
            nonlocal call_count
            call_count += 1
            result = real_fstat(fd)
            if call_count == 1:
                os.truncate(absolute_filepath, 500)
            return result

        with unittest.mock.patch("os.fstat", side_effect=flaky_fstat):
            self._copy_file(absolute_filepath)

        with open(os.path.join(self.new_root, "data/file.wt"), "rb") as f:
            copied = f.read()
        self.assertEqual(copied, contents[:500])
        self.hook.logger.warning.assert_called_once()

    def test_resumes_copy_when_file_grows_after_premature_eof(self):
        contents = b"a" * 1000
        appended = b"b" * 2000
        absolute_filepath = self._make_file("data/growing.wt", contents)

        # Shrink the file right after it's stat'd so the read loop hits a premature EOF,
        # then grow it again before the EOF recheck re-stats the file. copy_file() should
        # resume copying and pick up the appended bytes.
        real_fstat = os.fstat
        call_count = 0

        def flaky_fstat(fd):
            nonlocal call_count
            call_count += 1
            if call_count == 1:
                result = real_fstat(fd)
                os.truncate(absolute_filepath, 100)
            else:
                with open(absolute_filepath, "ab") as f:
                    f.write(appended)
                result = real_fstat(fd)
            return result

        with unittest.mock.patch("os.fstat", side_effect=flaky_fstat):
            self._copy_file(absolute_filepath)

        with open(os.path.join(self.new_root, "data/growing.wt"), "rb") as f:
            copied = f.read()
        self.assertEqual(copied, contents[:100] + appended)
        self.hook.logger.warning.assert_not_called()

    def test_copies_empty_file(self):
        absolute_filepath = self._make_file("data/empty.wt", b"")

        self._copy_file(absolute_filepath)

        with open(os.path.join(self.new_root, "data/empty.wt"), "rb") as f:
            self.assertEqual(f.read(), b"")
        self.hook.logger.warning.assert_not_called()


if __name__ == "__main__":
    unittest.main()
