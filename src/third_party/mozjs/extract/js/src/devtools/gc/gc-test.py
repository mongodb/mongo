# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Works with python2.6

import json
import math
import os
import sys
from operator import itemgetter
from subprocess import PIPE, Popen


class Test:
    def __init__(self, path, name):
        self.path = path
        self.name = name

    @classmethod
    def from_file(cls, path, name, options):
        return cls(path, name)


def find_tests(dir, substring=None):
    ans = []
    for dirpath, dirnames, filenames in os.walk(dir):
        if dirpath == ".":
            continue
        for filename in filenames:
            if not filename.endswith(".js"):
                continue
            test = os.path.join(dirpath, filename)
            if substring is None or substring in os.path.relpath(test, dir):
                ans.append([test, filename])
    return ans


def get_test_cmd(path):
    return [JS, "-f", path]


def avg(seq):
    return sum(seq) / len(seq)


def stddev(seq, mean):
    diffs = ((float(item) - mean) ** 2 for item in seq)
    return math.sqrt(sum(diffs) / len(seq))


def run_test(test):
    env = os.environ.copy()
    env["MOZ_GCTIMER"] = "stderr"
    cmd = get_test_cmd(test.path)
    total = []
    mark = []
    sweep = []
    close_fds = sys.platform != "win32"
    p = Popen(cmd, stdin=PIPE, stdout=PIPE, stderr=PIPE, close_fds=close_fds, env=env)
    out, err = p.communicate()
    out, err = out.decode(), err.decode()

    float_array = [float(_) for _ in err.split()]

    if len(float_array) == 0:
        print("Error: No data from application. Configured with --enable-gctimer?")
        sys.exit(1)

    for i, currItem in enumerate(float_array):
        if i % 3 == 0:
            total.append(currItem)
        else:
            if i % 3 == 1:
                mark.append(currItem)
            else:
                sweep.append(currItem)

    return max(total), avg(total), max(mark), avg(mark), max(sweep), avg(sweep)


def run_tests(tests, test_dir):
    bench_map = {}

    try:
        for i, test in enumerate(tests):
            filename_str = '"%s"' % test.name
            TMax, TAvg, MMax, MAvg, SMax, SAvg = run_test(test)
            bench_map[test.name] = [TMax, TAvg, MMax, MAvg, SMax, SAvg]
            fmt = '%20s: {"TMax": %4.1f, "TAvg": %4.1f, "MMax": %4.1f, "MAvg": %4.1f, "SMax": %4.1f, "SAvg": %4.1f}'  # NOQA: E501
            if i != len(tests) - 1:
                fmt += ","
            print(fmt % (filename_str, TMax, TAvg, MMax, MAvg, SMax, MAvg))
    except KeyboardInterrupt:
        print("fail")

    return dict(
        (
            filename,
            dict(TMax=TMax, TAvg=TAvg, MMax=MMax, MAvg=MAvg, SMax=SMax, SAvg=SAvg),
        )
        for filename, (TMax, TAvg, MMax, MAvg, SMax, SAvg) in bench_map.iteritems()
    )


def compare(current, baseline):
    percent_speedups = []
    for key, current_result in current.iteritems():
        try:
            baseline_result = baseline[key]
        except KeyError:
            print(key, "missing from baseline")
            continue

        val_getter = itemgetter("TMax", "TAvg", "MMax", "MAvg", "SMax", "SAvg")
        BTMax, BTAvg, BMMax, BMAvg, BSMax, BSAvg = val_getter(baseline_result)
        CTMax, CTAvg, CMMax, CMAvg, CSMax, CSAvg = val_getter(current_result)

        if CTAvg <= BTAvg:
            speedup = (CTAvg / BTAvg - 1) * 100
            result = "faster: %6.2f < baseline %6.2f (%+6.2f%%)" % (
                CTAvg,
                BTAvg,
                speedup,
            )
            percent_speedups.append(speedup)
        else:
            slowdown = (CTAvg / BTAvg - 1) * 100
            result = "SLOWER: %6.2f > baseline %6.2f (%+6.2f%%) " % (
                CTAvg,
                BTAvg,
                slowdown,
            )
            percent_speedups.append(slowdown)
        print("%30s: %s" % (key, result))
    if percent_speedups:
        print("Average speedup: %.2f%%" % avg(percent_speedups))


if __name__ == "__main__":
    script_path = os.path.abspath(__file__)
    script_dir = os.path.dirname(script_path)
    test_dir = os.path.join(script_dir, "tests")

    from optparse import OptionParser

    op = OptionParser(usage="%prog [options] JS_SHELL [TESTS]")

    op.add_option(
        "-b",
        "--baseline",
        metavar="JSON_PATH",
        dest="baseline_path",
        help="json file with baseline values to " "compare against",
    )

    (OPTIONS, args) = op.parse_args()
    if len(args) < 1:
        op.error("missing JS_SHELL argument")
    # We need to make sure we are using backslashes on Windows.
    JS, test_args = os.path.normpath(args[0]), args[1:]

    test_list = []
    bench_map = {}

    test_list = find_tests(test_dir)

    if not test_list:
        print >> sys.stderr, "No tests found matching command line arguments."
        sys.exit(0)

    test_list = [Test.from_file(tst, name, OPTIONS) for tst, name in test_list]

    try:
        print("{")
        bench_map = run_tests(test_list, test_dir)
        print("}")

    except OSError:
        if not os.path.exists(JS):
            print >> sys.stderr, "JS shell argument: file does not exist: '%s'" % JS
            sys.exit(1)
        else:
            raise

    if OPTIONS.baseline_path:
        baseline_map = []
        fh = open(OPTIONS.baseline_path, "r")
        baseline_map = json.load(fh)
        fh.close()
        compare(current=bench_map, baseline=baseline_map)
