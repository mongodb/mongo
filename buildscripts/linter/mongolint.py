#!/usr/bin/env python3
"""Simple C++ Linter."""

import argparse
import bisect
import io
import logging
import re
import sys

_RE_LINT = re.compile("//.*NOLINT")
_RE_COMMENT_STRIP = re.compile("//.*")
_RE_GENERIC_FCV_COMMENT = re.compile(r"\(Generic FCV reference\):")
GENERIC_FCV = [
    r"::kLatest",
    r"::kLastContinuous",
    r"::kLastLTS",
    r"::kUpgradingFromLastLTSToLatest",
    r"::kUpgradingFromLastContinuousToLatest",
    r"::kDowngradingFromLatestToLastLTS",
    r"::kDowngradingFromLatestToLastContinuous",
    r"\.isUpgradingOrDowngrading",
    r"->isUpgradingOrDowngrading",
    r"::kDowngradingFromLatestToLastContinuous",
    r"::kUpgradingFromLastLTSToLastContinuous",
]
_RE_GENERIC_FCV_REF = re.compile(r"(" + "|".join(GENERIC_FCV) + r")\b")
_RE_FEATURE_FLAG_IGNORE_FCV_CHECK_REF = re.compile(r"isEnabledAndIgnoreFCVUnsafe\(\)")
_RE_FEATURE_FLAG_IGNORE_FCV_CHECK_COMMENT = re.compile(r"\(Ignore FCV check\)")
_RE_HEADER = re.compile(r"\.(h|hpp)$")


class Linter:
    """Simple C++ Linter."""

    _license_header = """\
/**
 *    Copyright (C) {year}-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */""".splitlines()

    def __init__(self, file_name, raw_lines):
        """Create new linter."""
        self.file_name = file_name
        self.raw_lines = raw_lines
        self.clean_lines = []
        self.nolint_suppression = []
        self.generic_fcv_comments = []
        self.feature_flag_ignore_fcv_check_comments = []
        self._error_count = 0

    def lint(self):
        """Run linter, returning error count."""
        # steps:
        #  - Check for header
        #  - Check for NOLINT and Strip multi line comments
        #  - Run file-level checks
        #  - Run per-line checks

        start_line = self._check_for_server_side_public_license()

        self._check_newlines()
        self._check_and_strip_comments()

        # Line-level checks
        for linenum in range(start_line, len(self.clean_lines)):
            if not self.clean_lines[linenum]:
                continue

            # Relax the rule of commenting generic FCV references for files directly related to FCV
            # implementations.
            if "feature_compatibility_version" not in self.file_name:
                self._check_for_generic_fcv(linenum)

            # Don't check feature_flag.h/cpp where the function is defined and test files.
            if "feature_flag" not in self.file_name and "test" not in self.file_name:
                self._check_for_feature_flag_ignore_fcv(linenum)

        return self._error_count

    def _check_newlines(self):
        """Check that each source file ends with a newline character."""
        if self.raw_lines and self.raw_lines[-1][-1:] != "\n":
            self._error(
                len(self.raw_lines),
                "mongo/final_newline",
                "Files must end with a newline character.",
            )

    def _check_and_strip_comments(self):
        in_multi_line_comment = False

        for linenum, clean_line in enumerate(self.raw_lines):
            # Users can write NOLINT different ways
            # // NOLINT
            # // Some explanation NOLINT
            # so we need a regular expression
            if _RE_LINT.search(clean_line):
                self.nolint_suppression.append(linenum)

            if _RE_GENERIC_FCV_COMMENT.search(clean_line):
                self.generic_fcv_comments.append(linenum)

            if _RE_FEATURE_FLAG_IGNORE_FCV_CHECK_COMMENT.search(clean_line):
                self.feature_flag_ignore_fcv_check_comments.append(linenum)

            if not in_multi_line_comment:
                if "/*" in clean_line and "*/" not in clean_line:
                    in_multi_line_comment = True
                    clean_line = ""

                # Trim comments - approximately
                # Note, this does not understand if // is in a string
                # i.e. it will think URLs are also comments but this should be good enough to find
                # violators of the coding convention
                if "//" in clean_line:
                    clean_line = _RE_COMMENT_STRIP.sub("", clean_line)
            else:
                if "*/" in clean_line:
                    in_multi_line_comment = False

                clean_line = ""

            self.clean_lines.append(clean_line)

    def _license_error(self, linenum, msg, category="legal/license"):
        style_url = "https://github.com/mongodb/mongo/wiki/Server-Code-Style"
        self._error(linenum, category, "{} See {}".format(msg, style_url))
        return (False, linenum)

    def _check_for_server_side_public_license(self):
        """Return the number of the line at which the check ended."""
        src_iter = (x.rstrip() for x in self.raw_lines)
        linenum = 0
        for linenum, lic_line in enumerate(self._license_header):
            src_line = next(src_iter, None)
            if src_line is None:
                self._license_error(linenum, "Missing or incomplete license header.")
                return linenum
            lic_re = re.escape(lic_line).replace(r"\{year\}", r"\d{4}")
            if not re.fullmatch(lic_re, src_line):
                self._license_error(
                    linenum,
                    'Incorrect license header.\n  Expected: "{}"\n  Received: "{}"\n'.format(
                        lic_line, src_line
                    ),
                )
                return linenum

        # Warn if SSPL appears in Enterprise code, which has a different license.
        expect_sspl_license = (
            "enterprise" not in self.file_name and "modules/atlas" not in self.file_name
        )
        if not expect_sspl_license:
            self._license_error(
                linenum,
                "Incorrect license header found. Expected Enterprise license.",
                category="legal/enterprise_license",
            )
            return linenum
        return linenum

    def _check_for_generic_fcv(self, linenum):
        line = self.clean_lines[linenum]
        if _RE_GENERIC_FCV_REF.search(line):
            # Find the first generic FCV comment preceding the current line.
            i = bisect.bisect_right(self.generic_fcv_comments, linenum)
            if not i or self.generic_fcv_comments[i - 1] < (linenum - 10):
                self._error(
                    linenum,
                    "mongodb/fcv",
                    'Please add a comment containing "(Generic FCV reference):" within 10 lines '
                    + "before the generic FCV reference.",
                )

    def _check_for_feature_flag_ignore_fcv(self, linenum):
        line = self.clean_lines[linenum]
        if _RE_FEATURE_FLAG_IGNORE_FCV_CHECK_REF.search(line):
            # Find the first ignore FCV check comment preceding the current line.
            i = bisect.bisect_right(self.feature_flag_ignore_fcv_check_comments, linenum)
            if not i or self.feature_flag_ignore_fcv_check_comments[i - 1] < (linenum - 10):
                self._error(
                    linenum,
                    "mongodb/fcv",
                    'Please add a comment containing "(Ignore FCV check)":" within 10 lines '
                    + "before the isEnabledAndIgnoreFCVUnsafe() function call explaining why "
                    + "the FCV check is ignored.",
                )

    def _error(self, linenum, category, message):
        if linenum in self.nolint_suppression:
            return

        norm_file_name = self.file_name.replace("\\", "/")

        # Custom clang-tidy check tests purposefully produce errors for
        # tests to find. They should be ignored.
        if "mongo_tidy_checks/tests/" in norm_file_name:
            return

        if category == "legal/license":
            # Enterprise module does not have the SSPL license
            if "enterprise" in self.file_name or "modules/atlas" in self.file_name:
                return

            # The following files are in the src/mongo/ directory but technically belong
            # in src/third_party/ because their copyright does not belong to MongoDB.
            files_to_ignore = set(
                [
                    "src/mongo/scripting/mozjs/PosixNSPR.cpp",
                    "src/mongo/shell/linenoise.cpp",
                    "src/mongo/shell/linenoise.h",
                    "src/mongo/shell/mk_wcwidth.cpp",
                    "src/mongo/shell/mk_wcwidth.h",
                    "src/mongo/util/net/ssl_stream.cpp",
                    "src/mongo/util/scopeguard.h",
                    "src/mongo/util/varint_details.cpp",
                    "src/mongo/util/varint_details.h",
                ]
            )

            for file_to_ignore in files_to_ignore:
                if file_to_ignore in norm_file_name:
                    return

        # We count internally from 0 but users count from 1 for line numbers
        print("Error: %s:%d - %s - %s" % (self.file_name, linenum + 1, category, message))
        self._error_count += 1


def lint_file(file_name):
    """Lint file and print errors to console."""
    with io.open(file_name, encoding="utf-8") as file_stream:
        raw_lines = file_stream.readlines()

    linter = Linter(file_name, raw_lines)
    return linter.lint()


def main():
    # type: () -> int
    """Execute Main Entry point."""
    parser = argparse.ArgumentParser(description="MongoDB Simple C++ Linter.")

    parser.add_argument("file", type=str, help="C++ input file")

    parser.add_argument("-v", "--verbose", action="count", help="Enable verbose tracing")

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    try:
        error_count = lint_file(args.file)
        if error_count != 0:
            print('File "{}" failed with {} errors.'.format(args.file, error_count))
            return 1
        return 0
    except Exception as ex:
        print('Exception while checking file "{}": {}'.format(args.file, ex))
        return 2


if __name__ == "__main__":
    sys.exit(main())
