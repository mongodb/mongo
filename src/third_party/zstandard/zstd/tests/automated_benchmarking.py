# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ##########################################################################

import argparse
import glob
import json
import os
import time
import pickle as pk
import subprocess
import urllib.request


GITHUB_API_PR_URL = "https://api.github.com/repos/facebook/zstd/pulls?state=open"
GITHUB_URL_TEMPLATE = "https://github.com/{}/zstd"
RELEASE_BUILD = {"user": "facebook", "branch": "dev", "hash": None}

# check to see if there are any new PRs every minute
DEFAULT_MAX_API_CALL_FREQUENCY_SEC = 60
PREVIOUS_PRS_FILENAME = "prev_prs.pk"

# Not sure what the threshold for triggering alarms should be
# 1% regression sounds like a little too sensitive but the desktop
# that I'm running it on is pretty stable so I think this is fine
CSPEED_REGRESSION_TOLERANCE = 0.01
DSPEED_REGRESSION_TOLERANCE = 0.01


def get_new_open_pr_builds(prev_state=True):
    prev_prs = None
    if os.path.exists(PREVIOUS_PRS_FILENAME):
        with open(PREVIOUS_PRS_FILENAME, "rb") as f:
            prev_prs = pk.load(f)
    data = json.loads(urllib.request.urlopen(GITHUB_API_PR_URL).read().decode("utf-8"))
    prs = {
        d["url"]: {
            "user": d["user"]["login"],
            "branch": d["head"]["ref"],
            "hash": d["head"]["sha"].strip(),
        }
        for d in data
    }
    with open(PREVIOUS_PRS_FILENAME, "wb") as f:
        pk.dump(prs, f)
    if not prev_state or prev_prs == None:
        return list(prs.values())
    return [pr for url, pr in prs.items() if url not in prev_prs or prev_prs[url] != pr]


def get_latest_hashes():
    tmp = subprocess.run(["git", "log", "-1"], stdout=subprocess.PIPE).stdout.decode(
        "utf-8"
    )
    sha1 = tmp.split("\n")[0].split(" ")[1]
    tmp = subprocess.run(
        ["git", "show", "{}^1".format(sha1)], stdout=subprocess.PIPE
    ).stdout.decode("utf-8")
    sha2 = tmp.split("\n")[0].split(" ")[1]
    tmp = subprocess.run(
        ["git", "show", "{}^2".format(sha1)], stdout=subprocess.PIPE
    ).stdout.decode("utf-8")
    sha3 = "" if len(tmp) == 0 else tmp.split("\n")[0].split(" ")[1]
    return [sha1.strip(), sha2.strip(), sha3.strip()]


def get_builds_for_latest_hash():
    hashes = get_latest_hashes()
    for b in get_new_open_pr_builds(False):
        if b["hash"] in hashes:
            return [b]
    return []


def clone_and_build(build):
    if build["user"] != None:
        github_url = GITHUB_URL_TEMPLATE.format(build["user"])
        os.system(
            """
            rm -rf zstd-{user}-{sha} &&
            git clone {github_url} zstd-{user}-{sha} &&
            cd zstd-{user}-{sha} &&
            {checkout_command}
            make -j &&
            cd ../
        """.format(
                user=build["user"],
                github_url=github_url,
                sha=build["hash"],
                checkout_command="git checkout {} &&".format(build["hash"])
                if build["hash"] != None
                else "",
            )
        )
        return "zstd-{user}-{sha}/zstd".format(user=build["user"], sha=build["hash"])
    else:
        os.system("cd ../ && make -j && cd tests")
        return "../zstd"


def parse_benchmark_output(output):
    idx = [i for i, d in enumerate(output) if d == "MB/s"]
    return [float(output[idx[0] - 1]), float(output[idx[1] - 1])]


def benchmark_single(executable, level, filename):
    return parse_benchmark_output((
        subprocess.run(
            [executable, "-qb{}".format(level), filename], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        .stdout.decode("utf-8")
        .split(" ")
    ))


def benchmark_n(executable, level, filename, n):
    speeds_arr = [benchmark_single(executable, level, filename) for _ in range(n)]
    cspeed, dspeed = max(b[0] for b in speeds_arr), max(b[1] for b in speeds_arr)
    print(
        "Bench (executable={} level={} filename={}, iterations={}):\n\t[cspeed: {} MB/s, dspeed: {} MB/s]".format(
            os.path.basename(executable),
            level,
            os.path.basename(filename),
            n,
            cspeed,
            dspeed,
        )
    )
    return (cspeed, dspeed)


def benchmark(build, filenames, levels, iterations):
    executable = clone_and_build(build)
    return [
        [benchmark_n(executable, l, f, iterations) for f in filenames] for l in levels
    ]


def benchmark_dictionary_single(executable, filenames_directory, dictionary_filename, level, iterations):
    cspeeds, dspeeds = [], []
    for _ in range(iterations):
        output = subprocess.run([executable, "-qb{}".format(level), "-D", dictionary_filename, "-r", filenames_directory], stdout=subprocess.PIPE).stdout.decode("utf-8").split(" ")
        cspeed, dspeed = parse_benchmark_output(output)
        cspeeds.append(cspeed)
        dspeeds.append(dspeed)
    max_cspeed, max_dspeed = max(cspeeds), max(dspeeds)
    print(
        "Bench (executable={} level={} filenames_directory={}, dictionary_filename={}, iterations={}):\n\t[cspeed: {} MB/s, dspeed: {} MB/s]".format(
            os.path.basename(executable),
            level,
            os.path.basename(filenames_directory),
            os.path.basename(dictionary_filename),
            iterations,
            max_cspeed,
            max_dspeed,
        )
    )
    return (max_cspeed, max_dspeed)


def benchmark_dictionary(build, filenames_directory, dictionary_filename, levels, iterations):
    executable = clone_and_build(build)
    return [benchmark_dictionary_single(executable, filenames_directory, dictionary_filename, l, iterations) for l in levels]


def parse_regressions_and_labels(old_cspeed, new_cspeed, old_dspeed, new_dspeed, baseline_build, test_build):
    cspeed_reg = (old_cspeed - new_cspeed) / old_cspeed
    dspeed_reg = (old_dspeed - new_dspeed) / old_dspeed
    baseline_label = "{}:{} ({})".format(
        baseline_build["user"], baseline_build["branch"], baseline_build["hash"]
    )
    test_label = "{}:{} ({})".format(
        test_build["user"], test_build["branch"], test_build["hash"]
    )
    return cspeed_reg, dspeed_reg, baseline_label, test_label


def get_regressions(baseline_build, test_build, iterations, filenames, levels):
    old = benchmark(baseline_build, filenames, levels, iterations)
    new = benchmark(test_build, filenames, levels, iterations)
    regressions = []
    for j, level in enumerate(levels):
        for k, filename in enumerate(filenames):
            old_cspeed, old_dspeed = old[j][k]
            new_cspeed, new_dspeed = new[j][k]
            cspeed_reg, dspeed_reg, baseline_label, test_label = parse_regressions_and_labels(
                old_cspeed, new_cspeed, old_dspeed, new_dspeed, baseline_build, test_build
            )
            if cspeed_reg > CSPEED_REGRESSION_TOLERANCE:
                regressions.append(
                    "[COMPRESSION REGRESSION] (level={} filename={})\n\t{} -> {}\n\t{} -> {} ({:0.2f}%)".format(
                        level,
                        filename,
                        baseline_label,
                        test_label,
                        old_cspeed,
                        new_cspeed,
                        cspeed_reg * 100.0,
                    )
                )
            if dspeed_reg > DSPEED_REGRESSION_TOLERANCE:
                regressions.append(
                    "[DECOMPRESSION REGRESSION] (level={} filename={})\n\t{} -> {}\n\t{} -> {} ({:0.2f}%)".format(
                        level,
                        filename,
                        baseline_label,
                        test_label,
                        old_dspeed,
                        new_dspeed,
                        dspeed_reg * 100.0,
                    )
                )
    return regressions

def get_regressions_dictionary(baseline_build, test_build, filenames_directory, dictionary_filename, levels, iterations):
    old = benchmark_dictionary(baseline_build, filenames_directory, dictionary_filename, levels, iterations)
    new = benchmark_dictionary(test_build, filenames_directory, dictionary_filename, levels, iterations)
    regressions = []
    for j, level in enumerate(levels):
        old_cspeed, old_dspeed = old[j]
        new_cspeed, new_dspeed = new[j]
        cspeed_reg, dspeed_reg, baesline_label, test_label = parse_regressions_and_labels(
            old_cspeed, new_cspeed, old_dspeed, new_dspeed, baseline_build, test_build
        )
        if cspeed_reg > CSPEED_REGRESSION_TOLERANCE:
            regressions.append(
                "[COMPRESSION REGRESSION] (level={} filenames_directory={} dictionary_filename={})\n\t{} -> {}\n\t{} -> {} ({:0.2f}%)".format(
                    level,
                    filenames_directory,
                    dictionary_filename,
                    baseline_label,
                    test_label,
                    old_cspeed,
                    new_cspeed,
                    cspeed_reg * 100.0,
                )
            )
        if dspeed_reg > DSPEED_REGRESSION_TOLERANCE:
            regressions.append(
                "[DECOMPRESSION REGRESSION] (level={} filenames_directory={} dictionary_filename={})\n\t{} -> {}\n\t{} -> {} ({:0.2f}%)".format(
                    level,
                    filenames_directory,
                    dictionary_filename,
                    baseline_label,
                    test_label,
                    old_dspeed,
                    new_dspeed,
                    dspeed_reg * 100.0,
                )
            )
        return regressions


def main(filenames, levels, iterations, builds=None, emails=None, continuous=False, frequency=DEFAULT_MAX_API_CALL_FREQUENCY_SEC, dictionary_filename=None):
    if builds == None:
        builds = get_new_open_pr_builds()
    while True:
        for test_build in builds:
            if dictionary_filename == None:
                regressions = get_regressions(
                    RELEASE_BUILD, test_build, iterations, filenames, levels
                )
            else:
                regressions = get_regressions_dictionary(
                    RELEASE_BUILD, test_build, filenames, dictionary_filename, levels, iterations
                )
            body = "\n".join(regressions)
            if len(regressions) > 0:
                if emails != None:
                    os.system(
                        """
                        echo "{}" | mutt -s "[zstd regression] caused by new pr" {}
                    """.format(
                            body, emails
                        )
                    )
                    print("Emails sent to {}".format(emails))
                print(body)
        if not continuous:
            break
        time.sleep(frequency)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()

    parser.add_argument("--directory", help="directory with files to benchmark", default="golden-compression")
    parser.add_argument("--levels", help="levels to test e.g. ('1,2,3')", default="1")
    parser.add_argument("--iterations", help="number of benchmark iterations to run", default="1")
    parser.add_argument("--emails", help="email addresses of people who will be alerted upon regression. Only for continuous mode", default=None)
    parser.add_argument("--frequency", help="specifies the number of seconds to wait before each successive check for new PRs in continuous mode", default=DEFAULT_MAX_API_CALL_FREQUENCY_SEC)
    parser.add_argument("--mode", help="'fastmode', 'onetime', 'current', or 'continuous' (see README.md for details)", default="current")
    parser.add_argument("--dict", help="filename of dictionary to use (when set, this dictionary will be used to compress the files provided inside --directory)", default=None)

    args = parser.parse_args()
    filenames = args.directory
    levels = [int(l) for l in args.levels.split(",")]
    mode = args.mode
    iterations = int(args.iterations)
    emails = args.emails
    frequency = int(args.frequency)
    dictionary_filename = args.dict

    if dictionary_filename == None:
        filenames = glob.glob("{}/**".format(filenames))

    if (len(filenames) == 0):
        print("0 files found")
        quit()

    if mode == "onetime":
        main(filenames, levels, iterations, frequency=frequenc, dictionary_filename=dictionary_filename)
    elif mode == "current":
        builds = [{"user": None, "branch": "None", "hash": None}]
        main(filenames, levels, iterations, builds, frequency=frequency, dictionary_filename=dictionary_filename)
    elif mode == "fastmode":
        builds = [{"user": "facebook", "branch": "release", "hash": None}]
        main(filenames, levels, iterations, builds, frequency=frequency, dictionary_filename=dictionary_filename)
    else:
        main(filenames, levels, iterations, None, emails, True, frequency=frequency, dictionary_filename=dictionary_filename)
