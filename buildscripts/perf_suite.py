#!/usr/bin/env python
"""
    Copyright 2014 MongoDB Inc.

    This program is free software: you can redistribute it and/or  modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    As a special exception, the copyright holders give permission to link the
    code of portions of this program with the OpenSSL library under certain
    conditions as described in each individual source file and distribute
    linked combinations including the program with the OpenSSL library. You
    must comply with the GNU Affero General Public License in all respects
    for all of the code used other than as permitted herein. If you modify
    file(s) with this exception, you may extend this exception to your
    version of the file(s), but you are not obligated to do so. If you do not
    wish to do so, delete this exception statement from your version. If you
    delete this exception statement from all source files in the program,
    then also delete it in the license file.
"""

import os
import subprocess
import sys
import tempfile
import time

from optparse import OptionParser

class Benchmark(object):
    def __init__(self, name, executable, options):
        self._name = name
        self._executable = executable
        self._options = options
        self._ran = False

    def name(self):
        return self._name

    def run(self):
        self._result = subprocess.check_call([self._executable] + self._options)
        self._ran = True

    def result(self):
        if not self._ran:
            raise Exception("Cannot get result of a suite that hasn't ran")
        return self._result


def file_allocator_bench(variant, ntrials, megabytes, path, report_dir):
    executable = os.path.join("build", "file_allocator_bench")
    suitename = "FileAllocatorBenchmark-%s" % variant
    opts = []
    opts.append("--ntrials=%s" % ntrials)
    opts.append("--megabytes=%i" % megabytes)
    opts.append("--path=%s" % path)
    opts.append("--jsonReport=%s.json" % os.path.join(report_dir, variant))
    return Benchmark(suitename, executable, opts)


def configure_parser():
    parser = OptionParser()
    parser.add_option("-r", "--reportDir", type=str,
                      help="Where to write the report [default: %default]",
                      default=os.getcwd())
    parser.add_option("-w", "--workDir", type=str,
                      help="scratch space used by tests [default: %default]",
                      default=tempfile.gettempdir())
    return parser


# at some point this should read from a config file, but at this point
# its not worth overengineering
def make_suites(work_dir, report_dir):
    return [
        file_allocator_bench("16GB", ntrials=8, megabytes=1024*16,
                             path=work_dir, report_dir=report_dir),
        file_allocator_bench("1GB", ntrials=16, megabytes=1024,
                             path=work_dir, report_dir=report_dir),
        file_allocator_bench("128MB", ntrials=32, megabytes=128,
                             path=work_dir, report_dir=report_dir)
    ]


def main():
    parser = configure_parser()
    (options, args) = parser.parse_args()

    report_dir = options.reportDir
    work_dir = options.workDir

    # unique dir for this report
    unique_dir = os.path.join(report_dir, "perfsuite-run@%s" % time.time())
    os.makedirs(unique_dir)
    print "Writing results to %s" % unique_dir
    print

    for suite in make_suites(work_dir, unique_dir):

        print("Running suite - %s ..." % suite.name())
        start = time.time()
        suite.run()
        end = time.time()
        print("...Finished suite in %i seconds" % (end - start))

    sys.exit(0)

if __name__ == "__main__":
    main()
