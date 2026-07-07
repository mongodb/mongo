"""Unit tests for buildscripts/gather_failed_tests.py."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from buildscripts import gather_failed_tests as under_test


class TestGatherFailedTests(unittest.TestCase):
    def test_missing_build_events_file_skips_failed_test_gathering(self):
        original_cwd = os.getcwd()
        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                os.chdir(tmpdir)
                with patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}):
                    with patch("builtins.print") as mock_print:
                        under_test.main("missing_build_events.json")
            finally:
                os.chdir(original_cwd)

            self.assertFalse(Path(tmpdir, "dist-tests").exists())
            mock_print.assert_called_once()
            self.assertIn("missing_build_events.json", mock_print.call_args.args[0])


if __name__ == "__main__":
    unittest.main()
