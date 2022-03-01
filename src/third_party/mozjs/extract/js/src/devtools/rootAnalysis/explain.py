#!/usr/bin/python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.


from __future__ import print_function

import argparse
import re

from collections import defaultdict

parser = argparse.ArgumentParser(description="Process some integers.")
parser.add_argument("rootingHazards", nargs="?", default="rootingHazards.txt")
parser.add_argument("gcFunctions", nargs="?", default="gcFunctions.txt")
parser.add_argument("hazards", nargs="?", default="hazards.txt")
parser.add_argument("extra", nargs="?", default="unnecessary.txt")
parser.add_argument("refs", nargs="?", default="refs.txt")
args = parser.parse_args()

num_hazards = 0
num_refs = 0
num_missing = 0

try:
    with open(args.rootingHazards) as rootingHazards, open(
        args.hazards, "w"
    ) as hazards, open(args.extra, "w") as extra, open(args.refs, "w") as refs:
        current_gcFunction = None

        # Map from a GC function name to the list of hazards resulting from
        # that GC function
        hazardousGCFunctions = defaultdict(list)

        # List of tuples (gcFunction, index of hazard) used to maintain the
        # ordering of the hazards
        hazardOrder = []

        # Map from a hazardous GC function to the filename containing it.
        fileOfFunction = {}

        for line in rootingHazards:
            m = re.match(r"^Time: (.*)", line)
            mm = re.match(r"^Run on:", line)
            if m or mm:
                print(line, file=hazards)
                print(line, file=extra)
                print(line, file=refs)
                continue

            m = re.match(r"^Function.*has unnecessary root", line)
            if m:
                print(line, file=extra)
                continue

            m = re.match(r"^Function.*takes unsafe address of unrooted", line)
            if m:
                num_refs += 1
                print(line, file=refs)
                continue

            m = re.match(
                r"^Function.*has unrooted.*of type.*live across GC call '(.*?)' at (\S+):\d+$",
                line,
            )  # NOQA: E501
            if m:
                current_gcFunction = m.group(1)
                hazardousGCFunctions[current_gcFunction].append(line)
                hazardOrder.append(
                    (
                        current_gcFunction,
                        len(hazardousGCFunctions[current_gcFunction]) - 1,
                    )
                )
                num_hazards += 1
                fileOfFunction[current_gcFunction] = m.group(2)
                continue

            m = re.match(r"Function.*expected hazard.*but none were found", line)
            if m:
                num_missing += 1
                print(line + "\n", file=hazards)
                continue

            if current_gcFunction:
                if not line.strip():
                    # Blank line => end of this hazard
                    current_gcFunction = None
                else:
                    hazardousGCFunctions[current_gcFunction][-1] += line

        with open(args.gcFunctions) as gcFunctions:
            gcExplanations = {}  # gcFunction => stack showing why it can GC

            current_func = None
            explanation = None
            for line in gcFunctions:
                m = re.match(r"^GC Function: (.*)", line)
                if m:
                    if current_func:
                        gcExplanations[current_func] = explanation
                    current_func = None
                    if m.group(1) in hazardousGCFunctions:
                        current_func = m.group(1)
                        explanation = line
                elif current_func:
                    explanation += line
            if current_func:
                gcExplanations[current_func] = explanation

        for gcFunction, index in hazardOrder:
            gcHazards = hazardousGCFunctions[gcFunction]

            if gcFunction in gcExplanations:
                print(gcHazards[index] + gcExplanations[gcFunction], file=hazards)
            else:
                print(gcHazards[index], file=hazards)

except IOError as e:
    print("Failed: %s" % str(e))

print("Wrote %s" % args.hazards)
print("Wrote %s" % args.extra)
print("Wrote %s" % args.refs)
print(
    "Found %d hazards %d unsafe references %d missing"
    % (num_hazards, num_refs, num_missing)
)
