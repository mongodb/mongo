#!/usr/bin/env python3
"""Simple C++ Linter."""

import argparse
import bisect
import io
import logging
import re
import sys


def _make_polyfill_regex():
    polyfill_required_names = [
        '_',
        'adopt_lock',
        'async',
        'chrono',
        'condition_variable',
        'condition_variable_any',
        'cv_status',
        'defer_lock',
        'future',
        'future_status',
        'get_terminate',
        'launch',
        'lock_guard',
        'mutex',
        'notify_all_at_thread_exit',
        'packaged_task',
        'promise',
        'recursive_mutex',
        'set_terminate',
        'shared_lock',
        'shared_mutex',
        'shared_timed_mutex',
        'this_thread(?!::at_thread_exit)',
        'thread',
        'timed_mutex',
        'try_to_lock',
        'unique_lock',
        'unordered_map',
        'unordered_multimap',
        'unordered_multiset',
        'unordered_set',
    ]

    qualified_names = ['boost::' + name + "\\b" for name in polyfill_required_names]
    qualified_names.extend('std::' + name + "\\b" for name in polyfill_required_names)
    qualified_names_regex = '|'.join(qualified_names)
    return re.compile('(' + qualified_names_regex + ')')


_RE_LINT = re.compile("//.*NOLINT")
_RE_COMMENT_STRIP = re.compile("//.*")

_RE_PATTERN_MONGO_POLYFILL = _make_polyfill_regex()
_RE_UNSTRUCTURED_LOG = re.compile(r'\blogd\s*\(')
_RE_COLLECTION_SHARDING_RUNTIME = re.compile(r'\bCollectionShardingRuntime\b')
_RE_RAND = re.compile(r'\b(srand\(|rand\(\))')

_RE_GENERIC_FCV_COMMENT = re.compile(r'\(Generic FCV reference\):')
GENERIC_FCV = [
    r'::kLatest',
    r'::kLastContinuous',
    r'::kLastLTS',
    r'::kUpgradingFromLastLTSToLatest',
    r'::kUpgradingFromLastContinuousToLatest',
    r'::kDowngradingFromLatestToLastLTS',
    r'::kDowngradingFromLatestToLastContinuous',
    r'\.isUpgradingOrDowngrading',
    r'::kDowngradingFromLatestToLastContinuous',
    r'::kUpgradingFromLastLTSToLastContinuous',
]
_RE_GENERIC_FCV_REF = re.compile(r'(' + '|'.join(GENERIC_FCV) + r')\b')
_RE_FEATURE_FLAG_IGNORE_FCV_CHECK_REF = re.compile(r'isEnabledAndIgnoreFCVUnsafe\(\)')
_RE_FEATURE_FLAG_IGNORE_FCV_CHECK_COMMENT = re.compile(r'\(Ignore FCV check\)')
_RE_HEADER = re.compile(r'\.(h|hpp)$')

_CXX_COMPAT_HEADERS = [
    "assert", "ctype", "errno", "fenv", "float", "inttypes", "limits", "locale", "math", "setjmp",
    "signal", "stdarg", "stddef", "stdint", "stdio", "stdlib", "string", "time", "uchar", "wchar",
    "wctype"
]

# Successful matches `m` have a `m["base"]`, the basename of the file that was included.
_RE_CXX_COMPAT_HEADERS = re.compile(
    rf'# *include *((<)|("))(?P<base>{"|".join(_CXX_COMPAT_HEADERS)})\.h(?(2)>|")')


class Linter:
    """Simple C++ Linter."""

    _license_header = '''\
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
 */'''.splitlines()

    def __init__(self, file_name, raw_lines):
        """Create new linter."""
        self.file_name = file_name
        self.raw_lines = raw_lines
        self.clean_lines = []
        self.nolint_suppression = []
        self.generic_fcv_comments = []
        self.feature_flag_ignore_fcv_check_comments = []
        self._error_count = 0

        self.found_config_header = False

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

        # File-level checks
        self._check_macro_definition_leaks()

        # Line-level checks
        for linenum in range(start_line, len(self.clean_lines)):
            if not self.clean_lines[linenum]:
                continue

            self._check_for_mongo_polyfill(linenum)
            self._check_for_mongo_unstructured_log(linenum)
            self._check_for_mongo_config_header(linenum)
            self._check_for_collection_sharding_runtime(linenum)
            self._check_for_rand(linenum)
            self._check_for_c_stdlib_headers(linenum)

            # Relax the rule of commenting generic FCV references for files directly related to FCV
            # implementations.
            if not "feature_compatibility_version" in self.file_name:
                self._check_for_generic_fcv(linenum)

            # Don't check feature_flag.h/cpp where the function is defined and test files.
            if not "feature_flag" in self.file_name and not "test" in self.file_name:
                self._check_for_feature_flag_ignore_fcv(linenum)

        return self._error_count

    def _check_newlines(self):
        """Check that each source file ends with a newline character."""
        if self.raw_lines and self.raw_lines[-1][-1:] != '\n':
            self._error(
                len(self.raw_lines), 'mongo/final_newline',
                'Files must end with a newline character.')

    def _check_and_strip_comments(self):
        in_multi_line_comment = False

        for linenum in range(len(self.raw_lines)):
            clean_line = self.raw_lines[linenum]

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
                if "/*" in clean_line and not "*/" in clean_line:
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

    def _check_macro_definition_leaks(self):
        """Some header macros should appear in define/undef pairs."""
        if not _RE_HEADER.search(self.file_name):
            return
        # Naive check: doesn't consider `#if` scoping.
        # Assumes an #undef matches the nearest #define.
        for macro in ['MONGO_LOGV2_DEFAULT_COMPONENT']:
            re_define = re.compile(fr"^\s*#\s*define\s+{macro}\b")
            re_undef = re.compile(fr"^\s*#\s*undef\s+{macro}\b")
            def_line = None
            for idx, line in enumerate(self.clean_lines):
                if def_line is None:
                    if re_define.match(line):
                        def_line = idx
                else:
                    if re_undef.match(line):
                        def_line = None
            if def_line is not None:
                self._error(def_line, 'mongodb/undefmacro', f'Missing "#undef {macro}"')

    def _check_for_mongo_polyfill(self, linenum):
        line = self.clean_lines[linenum]
        match = _RE_PATTERN_MONGO_POLYFILL.search(line)
        if match:
            self._error(
                linenum, 'mongodb/polyfill',
                'Illegal use of banned name from std::/boost:: for "%s", use mongo::stdx:: variant instead'
                % (match.group(0)))

    def _check_for_mongo_unstructured_log(self, linenum):
        line = self.clean_lines[linenum]
        if _RE_UNSTRUCTURED_LOG.search(line) or 'doUnstructuredLogImpl' in line:
            self._error(
                linenum, 'mongodb/unstructuredlog', 'Illegal use of unstructured logging, '
                'this is only for local development use and should not be committed.')

    def _check_for_collection_sharding_runtime(self, linenum):
        line = self.clean_lines[linenum]
        if _RE_COLLECTION_SHARDING_RUNTIME.search(
                line
        ) and "/src/mongo/db/s/" not in self.file_name and "_test.cpp" not in self.file_name:
            self._error(
                linenum, 'mongodb/collection_sharding_runtime', 'Illegal use of '
                'CollectionShardingRuntime outside of mongo/db/s/; use CollectionShardingState '
                'instead; see src/mongo/db/s/collection_sharding_state.h for details.')

    def _check_for_rand(self, linenum):
        line = self.clean_lines[linenum]
        if _RE_RAND.search(line):
            self._error(linenum, 'mongodb/rand',
                        'Use of rand or srand, use <random> or PseudoRandom instead.')

    def _license_error(self, linenum, msg, category='legal/license'):
        style_url = 'https://github.com/mongodb/mongo/wiki/Server-Code-Style'
        self._error(linenum, category, '{} See {}'.format(msg, style_url))
        return (False, linenum)

    def _check_for_server_side_public_license(self):
        """Return the number of the line at which the check ended."""
        src_iter = (x.rstrip() for x in self.raw_lines)
        linenum = 0
        for linenum, lic_line in enumerate(self._license_header):
            src_line = next(src_iter, None)
            if src_line is None:
                self._license_error(linenum, 'Missing or incomplete license header.')
                return linenum
            lic_re = re.escape(lic_line).replace(r'\{year\}', r'\d{4}')
            if not re.fullmatch(lic_re, src_line):
                self._license_error(
                    linenum, 'Incorrect license header.\n'
                    '  Expected: "{}"\n'
                    '  Received: "{}"\n'.format(lic_line, src_line))
                return linenum

        # Warn if SSPL appears in Enterprise code, which has a different license.
        expect_sspl_license = "enterprise" not in self.file_name
        if not expect_sspl_license:
            self._license_error(linenum,
                                'Incorrect license header found. Expected Enterprise license.',
                                category='legal/enterprise_license')
            return linenum
        return linenum

    def _check_for_mongo_config_header(self, linenum):
        """Check for a config file."""
        if self.found_config_header:
            return

        line = self.clean_lines[linenum]
        self.found_config_header = line.startswith('#include "mongo/config.h"')

        if not self.found_config_header and "MONGO_CONFIG_" in line:
            self._error(linenum, 'build/config_h_include',
                        'MONGO_CONFIG define used without prior inclusion of config.h.')

    def _check_for_generic_fcv(self, linenum):
        line = self.clean_lines[linenum]
        if _RE_GENERIC_FCV_REF.search(line):
            # Find the first generic FCV comment preceding the current line.
            i = bisect.bisect_right(self.generic_fcv_comments, linenum)
            if not i or self.generic_fcv_comments[i - 1] < (linenum - 10):
                self._error(
                    linenum, 'mongodb/fcv',
                    'Please add a comment containing "(Generic FCV reference):" within 10 lines ' +
                    'before the generic FCV reference.')

    def _check_for_c_stdlib_headers(self, linenum):
        line = self.clean_lines[linenum]

        if match := _RE_CXX_COMPAT_HEADERS.match(line):
            self._error(
                linenum, 'mongodb/headers',
                f"Prohibited include of C header '<{match['base']}.h>'. " \
                f"Include C++ header '<c{match['base']}>' instead.")

    def _check_for_feature_flag_ignore_fcv(self, linenum):
        line = self.clean_lines[linenum]
        if _RE_FEATURE_FLAG_IGNORE_FCV_CHECK_REF.search(line):
            # Find the first ignore FCV check comment preceding the current line.
            i = bisect.bisect_right(self.feature_flag_ignore_fcv_check_comments, linenum)
            if not i or self.feature_flag_ignore_fcv_check_comments[i - 1] < (linenum - 10):
                self._error(
                    linenum, 'mongodb/fcv',
                    'Please add a comment containing "(Ignore FCV check)":" within 10 lines ' +
                    'before the isEnabledAndIgnoreFCVUnsafe() function call explaining why ' +
                    'the FCV check is ignored.')

    def _error(self, linenum, category, message):
        if linenum in self.nolint_suppression:
            return

        norm_file_name = self.file_name.replace('\\', '/')

        # Custom clang-tidy check tests purposefully produce errors for
        # tests to find. They should be ignored.
        if "mongo_tidy_checks/tests/" in norm_file_name:
            return

        if category == "legal/license":
            # Enterprise module does not have the SSPL license
            if "enterprise" in self.file_name:
                return

            # The following files are in the src/mongo/ directory but technically belong
            # in src/third_party/ because their copyright does not belong to MongoDB.
            files_to_ignore = set([
                'src/mongo/scripting/mozjs/PosixNSPR.cpp',
                'src/mongo/shell/linenoise.cpp',
                'src/mongo/shell/linenoise.h',
                'src/mongo/shell/mk_wcwidth.cpp',
                'src/mongo/shell/mk_wcwidth.h',
                'src/mongo/util/md5.cpp',
                'src/mongo/util/md5.h',
                'src/mongo/util/md5main.cpp',
                'src/mongo/util/net/ssl_stream.cpp',
                'src/mongo/util/scopeguard.h',
            ])

            for file_to_ignore in files_to_ignore:
                if file_to_ignore in norm_file_name:
                    return

        # We count internally from 0 but users count from 1 for line numbers
        print("Error: %s:%d - %s - %s" % (self.file_name, linenum + 1, category, message))
        self._error_count += 1


def lint_file(file_name):
    """Lint file and print errors to console."""
    with io.open(file_name, encoding='utf-8') as file_stream:
        raw_lines = file_stream.readlines()

    linter = Linter(file_name, raw_lines)
    return linter.lint()


def main():
    # type: () -> int
    """Execute Main Entry point."""
    parser = argparse.ArgumentParser(description='MongoDB Simple C++ Linter.')

    parser.add_argument('file', type=str, help="C++ input file")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    try:
        error_count = lint_file(args.file)
        if error_count != 0:
            print('File "{}" failed with {} errors.'.format(args.file, error_count))
            return 1
        return 0
    except Exception as ex:  # pylint: disable=broad-except
        print('Exception while checking file "{}": {}'.format(args.file, ex))
        return 2


if __name__ == '__main__':
    sys.exit(main())
