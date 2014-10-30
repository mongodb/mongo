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

import os, csv, operator
from time import mktime

try:
    from wt_nvd3_util import multiChart, parsetime
except ImportError:
    print >>sys.stderr, "Could not import wt_nvd3_util.py, it should be\
            in the same directory as %s" % sys.argv[0]
    sys.exit(-1)

def timesort(s):
    # Sort the timestr via its parsetime() value so that the year gets
    # added and it properly sorts.  Times are only %b %d %H:%M:%S and
    # may improperly sort if the data crosses a month boundary.
    t = operator.itemgetter('#time')
    timestr = t(s)
    return parsetime(timestr)

# Fixup the names and values in a dictionary read in from a csv file. One
# field must be "#time" - which is used to calculate the interval.
# Input is a dictionary, output is a list of dictionaries with a single entry.
def munge_dict(values_dict, abstime):
    sorted_values = sorted(values_dict, key=timesort)
    start_time = parsetime(sorted_values[0]['#time'])

    ret = []
    for v in sorted_values:
        if abstime:
            # Build the time series, milliseconds since the epoch
            v['#time'] = int(mktime(parsetime(v['#time']).timetuple())) * 1000
        else:
            # Build the time series as seconds since the start of the data
            v['#time'] = (parsetime(v['#time']) - start_time).seconds
        next_val = {}
        for title, value in v.items():
            if title.find('uS') != -1:
                title = title.replace('uS', 'ms')
                value = float(value) / 1000
            if title == 'totalsec':
                value = 0
            if title == 'checkpoints' and value == 'N':
                value = 0
            elif title.find('time') != -1:
                title = 'time'
            elif title.find('latency') == -1 and \
              title.find('checkpoints') == -1:
                title = title + ' (thousands)'
                value = float(value) / 1000
            next_val[title] = value
        ret.append(next_val)

    # After building the series, eliminate constants
    d0 = ret[0]
    for t0, v0 in d0.items():
        skip = True
        for d in ret:
            v = d[t0]
            if v != v0:
                skip = False
                break
        if skip:
            for dicts in ret:
                del dicts[t0]

    return ret

def addPlotsToChart(chart, graph_data, wtstat_chart = False):
    # Extract the times - they are the same for all lines.
    times = []
    for v in graph_data:
        times.append(v['time'])

    # Add a line to the graph for each field in the CSV file in alphabetical
    # order, so the key is sorted.
    for field in sorted(graph_data[0].keys()):
        if field == 'time':
            continue
        # Split the latency and non-latency measurements onto different scales
        axis = "1"
        if not wtstat_chart and field.find('latency') == -1:
            axis="2"
        ydata = []
        for v in graph_data:
            ydata.append(v[field])
        chart.add_serie(x=times, y=ydata, name=field, type="line", yaxis=axis)

# Input parameters are a chart populated with WiredTiger statistics and
# the directory where the wtperf monitor file can be found.
def addPlotsToStatsChart(chart, dirname, abstime):
    fname = os.path.join(dirname, 'monitor')
    try:
        with open(fname, 'rb') as csvfile:
            reader = csv.DictReader(csvfile)
            # Transform the data into something NVD3 can digest
            graph_data = munge_dict(reader, abstime)
    except IOError:
        print >>sys.stderr, "Could not open wtperf monitor file."
        sys.exit(-1)
    addPlotsToChart(chart, graph_data, 1)

def main():
    # Parse the command line
    import argparse

    parser = argparse.ArgumentParser(description='Create graphs from WiredTiger statistics.')
    parser.add_argument('--abstime', action='store_true',
        help='use absolute time on the x axis')
    parser.add_argument('--output', '-o', metavar='file',
        default='wtperf_stats.html', help='HTML output file')
    parser.add_argument('files', metavar='file', nargs='+',
        help='input monitor file generated by WiredTiger wtperf application')
    args = parser.parse_args()

    output_file = open(args.output, 'w')

    if len(args.files) != 1:
        print 'Script currently only supports a single monitor file'
        exit (1)

    chart_extra = {}
    # Add in the x axis if the user wants time.
    if args.abstime:
        chart_extra['x_axis_format'] = '%H:%M:%S'

    for f in args.files:
        with open(f, 'rb') as csvfile:
            reader = csv.DictReader(csvfile)
            # Transform the data into something NVD3 can digest
            graph_data = munge_dict(reader, args.abstime)

    chart = multiChart(name='wtperf',
                      height=450 + 10*len(graph_data[0].keys()),
                      resize=True,
                      x_is_date=args.abstime,
                      assets_directory='http://source.wiredtiger.com/graphs/',
                      **chart_extra)

    addPlotsToChart(chart, graph_data)

    chart.buildhtml()
    output_file.write(chart.htmlcontent)
    output_file.close()

if __name__ == '__main__':
    main()

