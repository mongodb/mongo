#!/usr/bin/python

import re
import argparse

parser = argparse.ArgumentParser(description='Process some integers.')
parser.add_argument('rootingHazards', nargs='?', default='rootingHazards.txt')
parser.add_argument('gcFunctions', nargs='?', default='gcFunctions.txt')
parser.add_argument('hazards', nargs='?', default='hazards.txt')
parser.add_argument('extra', nargs='?', default='unnecessary.txt')
parser.add_argument('refs', nargs='?', default='refs.txt')
args = parser.parse_args()

num_hazards = 0
num_refs = 0
try:
    with open(args.rootingHazards) as rootingHazards, \
        open(args.hazards, 'w') as hazards, \
        open(args.extra, 'w') as extra, \
        open(args.refs, 'w') as refs:
        current_gcFunction = None

        # Map from a GC function name to the list of hazards resulting from
        # that GC function
        hazardousGCFunctions = {}

        # List of tuples (gcFunction, index of hazard) used to maintain the
        # ordering of the hazards
        hazardOrder = []

        for line in rootingHazards:
            m = re.match(r'^Time: (.*)', line)
            mm = re.match(r'^Run on:', line)
            if m or mm:
                print >>hazards, line
                print >>extra, line
                print >>refs, line
                continue

            m = re.match(r'^Function.*has unnecessary root', line)
            if m:
                print >>extra, line
                continue

            m = re.match(r'^Function.*takes unsafe address of unrooted', line)
            if m:
                num_refs += 1
                print >>refs, line
                continue

            m = re.match(r"^Function.*has unrooted.*of type.*live across GC call ('?)(.*?)('?) at \S+:\d+$", line)
            if m:
                # Function names are surrounded by single quotes. Field calls
                # are unquoted.
                current_gcFunction = m.group(2)
                hazardousGCFunctions.setdefault(current_gcFunction, []).append(line)
                hazardOrder.append((current_gcFunction, len(hazardousGCFunctions[current_gcFunction]) - 1))
                num_hazards += 1
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
                m = re.match(r'^GC Function: (.*)', line)
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
                    print >>hazards, (gcHazards[index] + gcExplanations[gcFunction])
                else:
                    print >>hazards, gcHazards[index]

except IOError as e:
    print 'Failed: %s' % str(e)

print("Wrote %s" % args.hazards)
print("Wrote %s" % args.extra)
print("Wrote %s" % args.refs)
print("Found %d hazards and %d unsafe references" % (num_hazards, num_refs))
