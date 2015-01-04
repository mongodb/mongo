#!/usr/bin/env python
#
# Public Domain 2014-2015 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
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

import fileinput, os, shutil, sys, textwrap
from collections import defaultdict
from datetime import datetime
from subprocess import call

# Import the data describing which statistics should not be scaled
from stat_data import no_scale_per_second_list

TIMEFMT = "%b %d %H:%M:%S"
reportno = 0

# Plot a set of entries for a title.
def plot(title, values):
    global reportno

    # Ignore entries where the value never changes.
    skip = True
    t0, v0 = values[0]
    for t, v in values:
        if v != v0:
            skip = False
            break
    if skip:
        print 'skipping: ' + title
        return

    print 'building: ' + title
    reportno = reportno + 1
    num = "%03d" % reportno

    ylabel = 'Value'
    if title.split(' ')[1] != 'spinlock' and \
      title.split(' ', 1)[1] in no_scale_per_second_list:
        seconds = 1
    else:
        t1, v1 = values[1]
        seconds = (datetime.strptime(t1, TIMEFMT) -
            datetime.strptime(t0, TIMEFMT)).seconds
        if seconds == 0:
            seconds = 1
        ylabel += ' per second'

    # Write the raw data into a file for processing.
    of = open("reports/raw/report.%s.raw" % num, "w")
    for t, v in sorted(values):
        print >>of, "%s %g" % (t, float(v) / seconds)
    of.close()

    # Write a command file for gnuplot.
    of = open("gnuplot.cmd", "w")
    of.write('''
set terminal png nocrop size 800,600
set autoscale
set grid
set style data linespoints
set title "%(title)s"
set xlabel "Time"
set xtics rotate by -45
set xdata time
set timefmt "%(timefmt)s"
set format x "%(timefmt)s"
set ylabel "%(ylabel)s"
set yrange [0:]
set output 'reports/report.%(num)s.png'
plot "reports/raw/report.%(num)s.raw" using 1:4 notitle''' % {
        'num' : num,
        'timefmt' : TIMEFMT,
        'title' : title,
        'ylabel' : ylabel,
    })
    of.close()

    # Run gnuplot.
    call(["gnuplot", "gnuplot.cmd"])

    # Remove the command file.
    os.remove("gnuplot.cmd")

# Read the input into a dictionary of lists.
if sys.argv[1:] == []:
    print "usage: " + sys.argv[0] + " file ..."
    sys.exit(1)

# Remove and re-create the reports folder.
shutil.rmtree("reports", True)
os.makedirs("reports/raw")

d = defaultdict(list)
for line in fileinput.input(sys.argv[1:]):
    month, day, time, v, desc = line.strip('\n').split(" ", 4)
    d[desc].append((month + " " + day + " " + time, v))

# Plot each entry in the dictionary.
for items in sorted(d.iteritems()):
    plot('\\n'.join(l for l in textwrap.wrap(items[0], 60)), items[1])

