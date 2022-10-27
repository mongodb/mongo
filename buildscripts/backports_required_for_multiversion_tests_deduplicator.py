import json
import os
import subprocess
from typing import Any, Dict
import uuid
import argparse
import yaml

COMMENT = """# This file is intended to track tests that should be denylisted from multiversion testing due to
# changes that have not yet been backported to the last-lts or last-continuous development
# branches.
#
# Usage:
# Add the server ticket number and the path to the test file for the test you intend to denylist
# under the appropriate suite. Any test in a (ticket, test_file) pair that appears in this file but
# not in the last-lts or last-continuous branch version of this file indicates that a commit has
# not yet been backported to the last-lts or last-continuous branch and will be excluded from the
# multiversion suite corresponding to the root level suite key.
#
# Example: To prevent 'my_test_file.js' from running in the 'replica_sets_multiversion' suite with the last-continuous binary
# replica_sets_multiversion:
#   - ticket: SERVER-1000
#     test_file: jstests/core/my_test_file.js
#
# The above example will denylist jstests/core/my_test_file.js from the
# 'replica_sets_multiversion_gen' task until this file has been updated with the same
# (ticket, test_file) pair on the last-lts branch.
#
"""

parser = argparse.ArgumentParser(
    description='Compare two branches yaml file and create a non-overalpping set')

parser.add_argument(
    "--branches", type=str, help=
    "Comma seperated list of branches to compare, any entires in all of these branches can be removed.",
    default="v4.2,v4.4,v5.0,v6.0,v6.1,master")
parser.add_argument("--backport-file", type=str,
                    help="Yaml file to compare and remove overlapping set from.",
                    default="etc/backports_required_for_multiversion_tests.yml")
parser.add_argument("--remote", type=str, help="Remote to fetch from for comparision",
                    default="origin")
parser.add_argument("--dup-tickets-file", type=str,
                    help="File to store duplicate tickets while waiting",
                    default="duplicate_tickets.json")

args = parser.parse_args()

branches = args.branches.strip().split(",")
backport_file = args.backport_file
remote = args.remote
dup_tickets_file = args.dup_tickets_file


def create_duplicates_file():
    tests_by_branch: Dict[str, Dict[str, Any]] = {}
    for branch in branches:
        remote_branch = f"{remote}/{branch}"
        subprocess.run(["git", "fetch", remote, branch], check=True, capture_output=True)
        file_data_proc = subprocess.run(["git", "show", f"{remote_branch}:{backport_file}"],
                                        check=True, capture_output=True)
        multiversion_tests = yaml.safe_load(file_data_proc.stdout.decode('UTF-8'))
        tests_by_branch[branch] = multiversion_tests

    def get_tickets(tests):
        return [test["ticket"] for test in tests]

    overlapping_continous = set(
        get_tickets(tests_by_branch[branches[-1]]["last-continuous"]["all"]))
    overlapping_lts = set(get_tickets(tests_by_branch[branches[-1]]["last-lts"]["all"]))

    for branch in branches[:-1]:
        try:
            overlapping_continous.intersection_update(
                get_tickets(tests_by_branch[branch]["last-continuous"]["all"]))
            overlapping_lts.intersection_update(
                get_tickets(tests_by_branch[branch]["last-lts"]["all"]))
        except KeyError:
            # 4.4 and 4.2 have a different format
            overlapping_continous.intersection_update(get_tickets(tests_by_branch[branch]["all"]))
            overlapping_lts.intersection_update(get_tickets(tests_by_branch[branch]["all"]))

    overlapping_tickets = {
        "overlapping_continous": sorted(list(overlapping_continous)),
        "overlapping_lts": sorted(list(overlapping_lts)),
        "tests_by_branch": tests_by_branch,
    }
    with open(dup_tickets_file, 'w') as dup_tickets_fp:
        json.dump(overlapping_tickets, dup_tickets_fp, indent=4, sort_keys=True)

    print(f"Saved duplicate tickets to {dup_tickets_file}")


def update_backports():
    with open(dup_tickets_file, 'r') as dup_tickets_fp:
        overlapping_tickets = json.load(dup_tickets_fp)

    overlapping_continous = overlapping_tickets["overlapping_continous"]
    overlapping_lts = overlapping_tickets["overlapping_lts"]
    tests_by_branch: Dict[str, Dict[str, Any]] = overlapping_tickets["tests_by_branch"]

    for branch in branches:
        remote_branch = f"{remote}/{branch}"
        while input(f"Checking out {remote_branch} branch. Enter y to continue... ") != "y":
            pass

        tmp_branch_name = uuid.uuid4().hex

        subprocess.run(["git", "checkout", remote_branch, "-b", tmp_branch_name], check=True)

        if branch in ("v4.4", "v4.2"):
            continous_removed = [
                test for test in tests_by_branch[branch]["all"]
                if (not test["ticket"] in overlapping_continous) and (
                    not test["ticket"] in overlapping_lts)
            ]
            continous_removed.sort(key=lambda d: d['ticket'])
            tests_by_branch[branch]["all"] = continous_removed
        else:
            continous_removed = [
                test for test in tests_by_branch[branch]["last-continuous"]["all"]
                if not test["ticket"] in overlapping_continous
            ]
            continous_removed.sort(key=lambda d: d['ticket'])
            tests_by_branch[branch]["last-continuous"]["all"] = continous_removed
            lts_removed = [
                test for test in tests_by_branch[branch]["last-lts"]["all"]
                if not test["ticket"] in overlapping_lts
            ]
            lts_removed.sort(key=lambda d: d['ticket'])
            tests_by_branch[branch]["last-lts"]["all"] = lts_removed

        with open(backport_file, 'w') as backport_fp:
            backport_fp.write(COMMENT)
            yaml.dump(tests_by_branch[branch], backport_fp)

        while input(
                f"Please send the current change to the commit queue for {remote_branch}. Enter y to continue... "
        ) != "y":
            pass

        subprocess.run(["git", "checkout", remote_branch], check=True)
        subprocess.run(["git", "branch", "-D", tmp_branch_name], check=True)


if os.path.exists(dup_tickets_file):
    while True:
        prune_file = input(
            f"Duplicate tickets file exists {dup_tickets_file}, would you like to use this file to prune duplicate tickets (y/n): "
        )
        if prune_file == "n":
            create_duplicates_file()
            break
        elif prune_file == "y":
            break
else:
    create_duplicates_file()

update_backports()
