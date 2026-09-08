"""Unit tests for the simulate-crash hook."""

import os
import shutil
import tempfile
import unittest
import unittest.mock

from buildscripts.resmokelib.testing.hooks import simulate_crash


@unittest.skipUnless(
    hasattr(os, "sendfile"), "os.sendfile is not available on this platform (e.g. Windows)"
)
class TestCopyFile(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.new_root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.addCleanup(shutil.rmtree, self.new_root, ignore_errors=True)

    def _make_file(self, relative_path, contents):
        absolute_path = os.path.join(self.root, relative_path)
        os.makedirs(os.path.dirname(absolute_path), exist_ok=True)
        with open(absolute_path, "wb") as f:
            f.write(contents)
        # copy_file() assumes the destination dir already exists (capture_db's job).
        os.makedirs(os.path.dirname(os.path.join(self.new_root, relative_path)), exist_ok=True)
        return absolute_path

    def test_copies_full_file(self):
        contents = b"hello world" * 1000
        absolute_filepath = self._make_file("data/file.wt", contents)

        simulate_crash.SimulateCrash.copy_file(self.root, absolute_filepath, self.new_root)

        with open(os.path.join(self.new_root, "data/file.wt"), "rb") as f:
            self.assertEqual(f.read(), contents)

    def test_truncates_gracefully_when_file_shrinks_mid_copy(self):
        contents = b"x" * 1000
        absolute_filepath = self._make_file("data/file.wt", contents)

        # Truncate the file mid-copy, after it's been stat'd but before sendfile finishes.
        real_sendfile = os.sendfile
        call_count = 0

        def flaky_sendfile(out_fd, in_fd, offset, count):
            nonlocal call_count
            call_count += 1
            if call_count == 1:
                os.truncate(absolute_filepath, 500)
                return real_sendfile(out_fd, in_fd, offset, min(count, 500))
            return real_sendfile(out_fd, in_fd, offset, count)

        with unittest.mock.patch("os.sendfile", side_effect=flaky_sendfile):
            simulate_crash.SimulateCrash.copy_file(self.root, absolute_filepath, self.new_root)

        with open(os.path.join(self.new_root, "data/file.wt"), "rb") as f:
            copied = f.read()
        self.assertEqual(copied, contents[:500])

    def test_copies_empty_file(self):
        absolute_filepath = self._make_file("data/empty.wt", b"")

        simulate_crash.SimulateCrash.copy_file(self.root, absolute_filepath, self.new_root)

        with open(os.path.join(self.new_root, "data/empty.wt"), "rb") as f:
            self.assertEqual(f.read(), b"")


if __name__ == "__main__":
    unittest.main()
