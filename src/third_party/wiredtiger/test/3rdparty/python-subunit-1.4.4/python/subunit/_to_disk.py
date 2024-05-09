#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2009  Robert Collins <robertc@robertcollins.net>
#
#  Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
#  license at the users choice. A copy of both licenses are available in the
#  project source as Apache-2.0 and BSD. You may not use this file except in
#  compliance with one of these two licences.
#  
#  Unless required by applicable law or agreed to in writing, software
#  distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
#  license you chose for the specific language governing permissions and
#  limitations under that license.

import io
import json
import optparse
import os.path
import sys
from errno import EEXIST
from textwrap import dedent

from testtools import StreamToDict

from subunit.filters import run_tests_from_stream


def _allocate_path(root, sub):
    """Figoure a path for sub under root.

    If sub tries to escape root, squash it with prejuidice.

    If the path already exists, a numeric suffix is appended.
    E.g. foo, foo-1, foo-2, etc.

    :return: the full path to sub.
    """
    # subpathss are allowed, but not parents.
    candidate = os.path.realpath(os.path.join(root, sub))
    realroot = os.path.realpath(root)
    if not candidate.startswith(realroot):
        sub = sub.replace('/', '_').replace('\\', '_')
        return _allocate_path(root, sub)

    attempt = 0
    probe = candidate
    while os.path.exists(probe):
        attempt += 1
        probe = '%s-%s' % (candidate, attempt)
    return probe


def _open_path(root, subpath):
    name = _allocate_path(root, subpath)
    try:
        os.makedirs(os.path.dirname(name))
    except (OSError, IOError) as e:
        if e.errno != EEXIST:
            raise
    return io.open(name, 'wb')


def _json_time(a_time):
    if a_time is None:
        return a_time
    return str(a_time)


class DiskExporter:
    """Exports tests to disk."""

    def __init__(self, directory):
        self._directory = os.path.realpath(directory)

    def export(self, test_dict):
        id = test_dict['id']
        tags = sorted(test_dict['tags'])
        details = test_dict['details']
        status = test_dict['status']
        start, stop = test_dict['timestamps']
        test_summary = {}
        test_summary['id'] = id
        test_summary['tags'] = tags
        test_summary['status'] = status
        test_summary['details'] = sorted(details.keys())
        test_summary['start'] = _json_time(start)
        test_summary['stop'] = _json_time(stop)
        root = _allocate_path(self._directory, id)
        with _open_path(root, 'test.json') as f:
            maybe_str = json.dumps(
                test_summary, sort_keys=True, ensure_ascii=False)
            if not isinstance(maybe_str, bytes):
                maybe_str = maybe_str.encode('utf-8')
            f.write(maybe_str)
        for name, detail in details.items():
            with _open_path(root, name) as f:
                for chunk in detail.iter_bytes():
                    f.write(chunk)


def to_disk(argv=None, stdin=None, stdout=None):
    if stdout is None:
        stdout = sys.stdout
    if stdin is None:
        stdin = sys.stdin
    parser = optparse.OptionParser(
        description="Export a subunit stream to files on disk.",
        epilog=dedent("""\
            Creates a directory per test id, a JSON file with test
            metadata within that directory, and each attachment
            is written to their name relative to that directory.

            Global packages (no test id) are discarded.

            Exits 0 if the export was completed, or non-zero otherwise.
            """))
    parser.add_option(
        "-d", "--directory", help="Root directory to export to.",
        default=".")
    options, args = parser.parse_args(argv)
    if len(args) > 1:
        raise Exception("Unexpected arguments.")
    if len(args):
        source = io.open(args[0], 'rb')
    else:
        source = stdin
    exporter = DiskExporter(options.directory)
    result = StreamToDict(exporter.export)
    run_tests_from_stream(source, result, protocol_version=2)
    return 0

