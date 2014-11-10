#!/usr/bin/env python
#
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

import fileinput, os, re, shutil, sys, textwrap
from collections import defaultdict
from time import mktime
from subprocess import call

# Make sure Python can find files in the tools directory
tool_dir = os.path.split(sys.argv[0])[0]
sys.path.append(tool_dir)
# Make sure Python finds the NVD3 in our third party directory, to
# avoid compatability issues
sys.path.append(os.path.join(tool_dir, "third_party"))

try:
    from stat_data import no_scale_per_second_list, no_clear_list
except ImportError:
    print >>sys.stderr, "Could not import stat_data.py, it should be\
            in the same directory as %s" % sys.argv[0]
    sys.exit(-1)

try:
    from wtperf_stats import addPlotsToStatsChart
except ImportError:
    print >>sys.stderr, "Could not import wtperf_stats.py, it should be\
            in the same directory as %s" % sys.argv[0]
    sys.exit(-1)

try:
    from wt_nvd3_util import multiChart, parsetime
except ImportError:
    print >>sys.stderr, "Could not import wt_nvd3_util.py, it should be\
            in the same directory as %s" % sys.argv[0]
    sys.exit(-1)

try:
    from nvd3 import lineChart, lineWithFocusChart
except ImportError:
    print >>sys.stderr, "Could not import nvd3 it should be installed locally"
    sys.exit(-1)

if sys.version_info<(2,7,0):
    print >>sys.stderr, "You need python 2.7 or later to run this script"
    sys.exit(-1)

# Plot a set of entries for a title.
def munge(title, values):
    t0, v0 = values[0]
    start_time = parsetime(t0)

    ylabel = ' '.join(title.split(' ')).lower()
    if title.split(' ')[1] != 'spinlock' and \
      title.split(' ', 1)[1] in no_scale_per_second_list:
        seconds = 1
    else:
        t1, v1 = values[1]
        seconds = (parsetime(t1) - start_time).seconds
        ylabel += ' per second'
        if seconds == 0:
            seconds = 1

    stats_cleared = False
    if args.clear or title.split(' ', 1)[1] in no_clear_list:
        stats_cleared = True

    # Split the values into a dictionary of y-axis values keyed by the x axis
    ydata = {}
    last_value = 0.0
    for t, v in sorted(values):
        if args.abstime:
            # Build the time series, milliseconds since the epoch
            x = int(mktime(parsetime(t).timetuple())) * 1000
        else:
            # Build the time series as seconds since the start of the data
            x = (parsetime(t) - start_time).seconds

        float_v = float(v)
        if not stats_cleared:
            float_v = float_v - last_value
            # Sometimes WiredTiger stats go backwards without clear, assume
            # that means nothing happened
            if float_v < 0:
                float_v = 0.0
            last_value = float(v)
        ydata[x] = float_v / seconds

    return ylabel, ydata

# Parse the command line
import argparse

parser = argparse.ArgumentParser(description='Create graphs from WiredTiger statistics.')
parser.add_argument('--abstime', action='store_true',
    help='use absolute time on the x axis')
parser.add_argument('--clear', action='store_true',
    help='WiredTiger stats gathered with clear set')
parser.add_argument('--focus', action='store_true',
    help='generate a chart with focus slider')
parser.add_argument('--include', '-I', metavar='regexp',
    type=re.compile, action='append',
    help='include series with titles matching the specifed regexp')
parser.add_argument('--list', action='store_true',
    help='list the series that would be displayed')
parser.add_argument('--output', '-o', metavar='file', default='wtstats.html',
    help='HTML output file')
parser.add_argument('--right', '-R', metavar='regexp',
    type=re.compile, action='append',
    help='use the right axis for series with titles matching the specifed regexp')
parser.add_argument('--wtperf', '-w', action='store_true',
    help='Plot wtperf statistics on the same graph')
parser.add_argument('files', metavar='file', nargs='+',
    help='input files generated by WiredTiger statistics logging')
args = parser.parse_args()

# Don't require users to specify regexps twice for right axis
if args.focus and args.right:
    print >>sys.stderr, "focus charts cannot have a right-hand y-axis"
    sys.exit(-1)

# Don't require users to specify regexps twice for right axis
if args.include and args.right:
    args.include += args.right

# Read the input file(s) into a dictionary of lists.
d = defaultdict(list)
for f in args.files:
    for line in open(f, 'rU'):
        month, day, time, v, title = line.strip('\n').split(" ", 4)
        d[title].append((month + " " + day + " " + time, v))

# Process the series, eliminate constants
for title, values in sorted(d.iteritems()):
    skip = True
    t0, v0 = values[0]
    for t, v in values:
        if v != v0:
            skip = False
            break
    if skip:
        #print "Skipping", title
        del d[title]

# Common prefix / suffix elimination
prefix = suffix = None

def common_prefix(a, b):
    while not b.startswith(a):
        a = a[:-1]
    return a

def common_suffix(a, b):
    while not a.endswith(b):
        b = b[1:]
    return b

# Split out the data, convert timestamps
results = []
for title, values in sorted(d.iteritems()):
    title, ydata = munge(title, values)
    # Ignore entries if a list of regular expressions was given
    if args.include and not [r for r in args.include if r.search(title)]:
        continue
    yaxis = args.right and [r for r in args.right if r.search(title)]
    prefix = title if prefix is None else common_prefix(prefix, title)
    suffix = title if suffix is None else common_suffix(title, suffix)
    results.append((title, yaxis, ydata))

# Process titles, eliminate common prefixes and suffixes
if prefix or suffix:
    new_results = []
    for title, yaxis, ydata in results:
        title = title[len(prefix):]
        if suffix:
            title = title[:-len(suffix)]
        new_results.append((title, yaxis, ydata))
    results = new_results

# Dump the results as a CSV file
#print '"time", ' + ', '.join('"%s"' % title for title, values in ydata)
#for i in xrange(len(xdata)):
#    print '%d, %s' % (xdata[i], ', '.join('%g' % values[i] for title, values in ydata))

# Are we just listing the results?
if args.list:
    for title, yaxis, ydata in results:
        print title
    sys.exit(0)

# Figure out the full set of x axis values
xdata = sorted(set(k for k in ydata.iterkeys() for ydata in results))

# open the output file
output_file = open(args.output, 'w')
#---------------------------------------
if args.right:
    charttype = multiChart
elif args.focus:
    charttype = lineWithFocusChart
else:
    charttype = lineChart

chart_extra = {}
# Add in the x axis if the user wants time.
if args.abstime:
    chart_extra['x_axis_format'] = '%H:%M:%S'

# Create the chart, add the series
chart = charttype(name='statlog', height=450+10*len(results), resize=True, x_is_date=args.abstime, y_axis_format='g', assets_directory='http://source.wiredtiger.com/graphs/', **chart_extra)

for title, yaxis, ydata in results:
    chart.add_serie(x=xdata, y=(ydata.get(x, 0) for x in xdata), name=title,
                    type="line", yaxis="2" if yaxis else "1")

if args.wtperf:
    addPlotsToStatsChart(chart, os.path.dirname(args.files[0]), args.abstime)

chart.buildhtml()
output_file.write(chart.htmlcontent)

#close Html file
output_file.close()
