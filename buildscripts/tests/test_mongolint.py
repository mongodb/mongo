"""Unit tests for buildscripts/linter/mongolint.py license header checks."""

import unittest

from buildscripts.linter.mongolint import Linter

# The traditional SSPL license header, with a concrete year filled in.
TRADITIONAL_HEADER = "\n".join(Linter._sspl_license_header).format(year=2024) + "\n"

# The SPDX-style license header.
SPDX_HEADER = "\n".join(Linter._spdx_license_header) + "\n"

# A trivial body so files are non-empty after the header.
BODY = "\nint main() {}\n"


def _lint_license(file_name, contents):
    """Run just the license check on file contents and return the error count."""
    raw_lines = contents.splitlines(keepends=True)
    linter = Linter(file_name, raw_lines)
    linter._check_for_server_side_public_license()
    return linter._error_count


class TestLicenseHeader(unittest.TestCase):
    def test_traditional_header_passes(self):
        self.assertEqual(_lint_license("src/mongo/foo.cpp", TRADITIONAL_HEADER + BODY), 0)

    def test_spdx_header_passes(self):
        self.assertEqual(_lint_license("src/mongo/foo.cpp", SPDX_HEADER + BODY), 0)

    def test_missing_header_fails(self):
        self.assertGreater(_lint_license("src/mongo/foo.cpp", BODY), 0)

    def test_wrong_header_fails(self):
        wrong = "// Some other license\n// Not SSPL at all\n" + BODY
        self.assertGreater(_lint_license("src/mongo/foo.cpp", wrong), 0)

    def test_empty_file_fails(self):
        self.assertGreater(_lint_license("src/mongo/foo.cpp", ""), 0)

    def test_traditional_header_rejects_missing_year(self):
        # The {year} placeholder must be a 4-digit year; text there is not accepted.
        bad_year = "\n".join(Linter._sspl_license_header).format(year="XXXX") + "\n"
        self.assertGreater(_lint_license("src/mongo/foo.cpp", bad_year + BODY), 0)

    def test_enterprise_traditional_header_flagged(self):
        # Enterprise code should use the enterprise license, so an SSPL header is an error.
        # legal/license errors are suppressed for enterprise files, but the enterprise-license
        # check emits a legal/enterprise_license error instead.
        self.assertGreater(
            _lint_license("src/mongo/db/modules/enterprise/foo.cpp", TRADITIONAL_HEADER + BODY),
            0,
        )

    def test_enterprise_spdx_header_flagged(self):
        self.assertGreater(
            _lint_license("src/mongo/db/modules/enterprise/foo.cpp", SPDX_HEADER + BODY),
            0,
        )


if __name__ == "__main__":
    unittest.main()
