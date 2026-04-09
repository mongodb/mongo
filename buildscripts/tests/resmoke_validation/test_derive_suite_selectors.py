"""Tests that derive_suite_selectors produces valid Bazel labels."""

import re
import sys
import unittest
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(_REPO_ROOT / "bazel/resmoke"))

from derive_suite_selectors import gen_suite_selectors  # noqa: E402

# A valid absolute Bazel label has the form //package:target or //package.
_VALID_LABEL_RE = re.compile(r"^//[A-Za-z0-9/_.\-]*(:[A-Za-z0-9/_.+\-]*)?$")


def _is_valid_bazel_label(label: str) -> bool:
    """Return True iff label is a syntactically valid absolute Bazel label."""
    return bool(_VALID_LABEL_RE.match(label))


class TestDeriveSuiteSelectorsLabels(unittest.TestCase):
    """Validate that gen_suite_selectors always emits well-formed Bazel labels."""

    @classmethod
    def setUpClass(cls):
        cls.result = gen_suite_selectors(_REPO_ROOT)
        output_file = _REPO_ROOT / "bazel/resmoke/.resmoke_suites_derived.bzl"
        cls.output_file = output_file
        cls.content = output_file.read_text() if output_file.exists() else ""

    def test_succeeds(self):
        self.assertTrue(
            self.result["ok"],
            msg=f"gen_suite_selectors failed: {self.result.get('err')}",
        )

    def test_output_file_exists(self):
        self.assertTrue(
            self.output_file.exists(),
            msg=f"Expected output file not found: {self.output_file}",
        )

    def test_suite_keys_are_valid_bazel_labels(self):
        # Keys look like:  "//buildscripts/resmokeconfig:suites/auth.yml": [
        keys = re.findall(r'"(//[^"]+)":\s*\[', self.content)
        invalid = [k for k in keys if not _is_valid_bazel_label(k)]
        self.assertFalse(
            invalid,
            msg="Suite config label keys that are not valid Bazel labels:\n"
            + "\n".join(f"  {k}" for k in invalid),
        )

    def test_src_values_are_valid_bazel_labels(self):
        # Values look like:  "//jstests/core:all_javascript_files",
        values = re.findall(r'^\s+"(//[^"]+)",', self.content, re.MULTILINE)
        invalid = [v for v in values if not _is_valid_bazel_label(v)]
        self.assertFalse(
            invalid,
            msg="Label values that are not valid Bazel labels:\n"
            + "\n".join(f"  {v}" for v in invalid),
        )


if __name__ == "__main__":
    unittest.main()
