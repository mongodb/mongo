#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

import fileinput, os, shutil, sys
from collections import defaultdict
from subprocess import call

# Plot a set of entries for a title.
def plot(title, entries, num):
    # Ignore entries where the value never changes.
    skip = 1
    v = entries[0][1]
    for entry in entries:
        if v != entry[1]:
            skip = 0
            break
    if skip == 1:
        print '\tskipping ' + title
        return

    print 'building ' + title

    # Write the raw data into a file for processing.
    of = open("reports/report." + num + ".raw", "w")
    for entry in sorted(entries):
        of.write(" ".join(entry) + "\n")
    of.close()

    # Write a command file for gnuplot.
    of = open("gnuplot.cmd", "w")
    of.write("set terminal png nocrop\n")
    of.write("set autoscale\n")
    of.write("set grid\n")
    of.write("set style data linespoints\n")
    of.write("set title \"" + title + "\"\n")
    of.write("set xlabel \"Time\"\n")
    of.write("set xtics rotate by -45\n")
    of.write("set xdata time\n")
    of.write("set timefmt \"%b %d %H:%M:%S\"\n")
    of.write("set format x \"%b %d %H:%M:%S\"\n")
    of.write("set ylabel \"Value\"\n")
    of.write("set yrange [0:]\n")
    of.write("set output 'reports/report." + num + ".png'\n")
    of.write("plot \"reports/report." + num + ".raw\" using 1:4 notitle\n")
    of.close()

    # Run gnuplot.
    call(["gnuplot", "gnuplot.cmd"])

    # Remove the command file.
    os.remove("gnuplot.cmd")

# Remove and re-create the reports folder.
shutil.rmtree("reports", True)
os.mkdir("reports")

# Read the input into a dictionary of lists.
if sys.argv[1:] == []:
    print "usage: " + sys.argv[0] + " file ..."
    sys.exit(1)
d = defaultdict(list)
for line in fileinput.input(sys.argv[1:]):
    s = line.strip('\n').split(" ", 4)
    d[s[4]].append([" ".join([s[0], s[1], s[2]]), s[3]])

# Plot each entry in the dictionary.
rno = 0
for entry in d.iteritems():
    rno = rno + 1
    plot(entry[0], entry[1], "%03d" % rno)
