#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import os, re, sys
from collections import defaultdict
from glob import glob
import json
from datetime import datetime

try:
    from stat_data \
        import groups, no_scale_per_second_list, no_clear_list, prefix_list
except ImportError:
    print >>sys.stderr, "Could not import stat_data.py, it should be" \
        "in the same directory as %s" % sys.argv[0]
    sys.exit(-1)

thisyear = datetime.today().year
def parsetime(s):
    return datetime.strptime(s, "%b %d %H:%M:%S").replace(year=thisyear)

if sys.version_info<(2,7,0):
    print >>sys.stderr, "You need python 2.7 or later to run this script"
    sys.exit(-1)

# Plot a set of entries for a title.
def munge(args, title, values):
    t0, v0 = values[0]
    start_time = parsetime(t0)

    ylabel = ' '.join(title.split(' ')).lower()
    if title.split(' ')[1] != 'spinlock' and \
      title.split(' ', 1)[1] in no_scale_per_second_list:
        seconds = 1
    elif 'wtperf' in title and 'per second' not in title:
        seconds = 1
    else:
        t1, v1 = values[1]
        seconds = (parsetime(t1) - start_time).seconds
        if not ylabel.endswith('per second'):
            ylabel += ' per second'
        if seconds == 0:
            seconds = 1

    stats_cleared = False
    if args.clear or title.split(' ', 1)[1] in no_clear_list or 'wtperf' in title:
        stats_cleared = True

    # Split the values into a dictionary of y-axis values keyed by the x axis
    ydata = {}
    last_value = 0.0
    for t, v in sorted(values):
        float_v = float(v)
        if not stats_cleared:
            float_v = float_v - last_value
            # Sometimes WiredTiger stats go backwards without clear, assume
            # that means nothing happened
            if float_v < 0:
                float_v = 0.0
            last_value = float(v)
        ydata[t] = float_v / seconds

    return ylabel, ydata


# Parse the command line
import argparse

def common_prefix(a, b):
    """ compute longest common prefix of a and b """
    while not b.startswith(a):
        a = a[:-1]
    return a


def common_suffix(a, b):
    """ compute longest common suffix of a and b """
    while not a.endswith(b):
        b = b[1:]
    return b


def parse_wtstats_file(file, result):
    """ parse wtstats file, one stat per line, example format:
           Dec 05 14:43:14 0 /data/b block-manager: mapped bytes read
    """
    print 'Processing wtstats file: ' + file

    # Parse file
    for line in open(file, 'rU'):
        month, day, time, v, title = line.strip('\n').split(" ", 4)
        result[title].append((month + " " + day + " " + time, v))



def parse_wtperf_file(file, result):
    """ parse wtperf file, all stats on single line, example format:
           Feb 13 17:55:14,0,0,156871,0,N,0,0,0,49,6,6146,0,0,0
    """
    print 'Processing wtperf file: ' + file
    fh = open(file, 'rU')

    # first line contains headings, replace microseconds with milliseconds
    headings = fh.next().strip('\n').split(',')[1:]
    headings = map(lambda h: h.replace('(uS)', ' (ms)'), headings)

    # parse rest of file
    for line in fh:
        month, day, time, values = re.split(r'[ ,]', line.strip('\n'), 3)
        values = values.split(',')
        for i, v in enumerate(values):
            if v == 'N': 
                v = 0
            # convert us to ms
            if '(ms)' in headings[i]:
                v = float(v) / 1000.0
            result['wtperf: ' + headings[i]].append((month + " " + day + " " + time, v))


def skip_constants(result):
    # Process the series, eliminate constants, delete totalsec for wtperf
    items = list(result.iteritems())

    for title, values in items:
        skip = True
        t0, v0 = values[0]
        for t, v in values:
            if v != v0:
                skip = False
                break
        
        if title == 'wtperf: totalsec':
            skip = True

        if skip:
            del result[title]

    return result


def parse_files(files_or_dir):
    """ walk through file list or directory and parse according to file type (wtstats / wtperf). """

    result = defaultdict(list)

    for f in files_or_dir:
        if os.path.isfile(f):
            # peek at first line to determine type
            with open(f, 'rU') as fh:
                line = fh.readline()
                if line.startswith('#time'):
                    parse_wtperf_file(f, result)
                else: 
                    parse_wtstats_file(f, result)
            
        elif os.path.isdir(f):
            for s in glob(os.path.join(f, 'WiredTigerStat*')):
                parse_wtstats_file(s, result)

            for s in glob(os.path.join(f, 'monitor*')):
                parse_wtperf_file(s, result)

    return result



def output_series(results, args, prefix=None, grouplist=[]):
    """ Write the data into the html template """

    # add .html ending if not present
    filename, ext = os.path.splitext(args.output)
    if ext == '':
        ext = '.html'

    # open the output file based on prefix
    if prefix == None:
        outputname = filename + ext
    elif len(grouplist) == 0:
        outputname = filename +'.' + prefix + ext
    else:
        outputname = filename +'.group.' + prefix + ext

    if prefix != None and len(grouplist) == 0:
        this_series = []
        for title, ydata in results:
            if not prefix in title:
                continue
            #print 'Appending to dataset: ' + title
            this_series.append((title, ydata))
    elif prefix != None and len(grouplist) > 0:
        this_series = []
        for title, ydata in results:
            for subgroup in grouplist:
                if not subgroup in title:
                    continue
                # print 'Appending to dataset: ' + title
                this_series.append((title, ydata))
    else:
        this_series = results

    if len(this_series) == 0:
        print 'Output: ' + outputname + ' has no data.  Do not create.'
        return


    json_output = { "series": [] }

    for title, ydata in this_series:
        json_output["series"].append({
            "key": title,
            "values": ydata,
        });
    
    # load template
    this_path = os.path.dirname(os.path.realpath(__file__))
    srcfile = os.path.join(this_path, 'wtstats.html.template')
    try: 
        srcfile = open(srcfile)
        contents = srcfile.read()
    except IOError: 
        print >>sys.stderr, "Cannot find template file 'wtstats.html." \
            "template'. See ./template/README.md for more information."
        sys.exit(-1)  

    srcfile.close()

    # if --json write data to <filename>.json
    if args.json:
        jsonfile = filename + '.json'
        with open(jsonfile, 'w') as f:
            json.dump(json_output, f)
            print "created %s" % jsonfile

    # write output file
    dstfile = open(outputname, 'wt')
    replaced_contents = contents.replace('"### INSERT DATA HERE ###"', 
        json.dumps(json_output))
    dstfile.write(replaced_contents)
    dstfile.close()
    print "created %s" % dstfile.name




def main():   
    parser = argparse.ArgumentParser(description='Create graphs from' \
        'WiredTiger statistics.')
    parser.add_argument('--all', '-A', action='store_true',
        help='generate separate html files for each stats group')
    parser.add_argument('--clear', action='store_true',
        help='WiredTiger stats gathered with clear set')
    parser.add_argument('--include', '-I', metavar='regexp',
        type=re.compile, action='append',
        help='only include series with titles matching regexp')
    parser.add_argument('--list', action='store_true',
        help='only list the parsed series, does not create html file')
    parser.add_argument('--output', '-o', metavar='file', default='wtstats',
        help='HTML output file prefix')
    parser.add_argument('--json', action='store_true', 
        help='additionally output data series in json format')
    parser.add_argument('files', metavar='file', nargs='+',
        help='input files or directories generated by WiredTiger statistics' \
        'logging')
    args = parser.parse_args()

    # Parse files or directory and skip constants
    parsed = skip_constants(parse_files(args.files))

    # filter results based on --include, compute common prefix and suffix
    results = []
    prefix = suffix = None

    for title, values in sorted(parsed.iteritems()):
        title, ydata = munge(args, title, values)

        # ignore entries if a list of regular expressions was given
        if args.include and not [r for r in args.include if r.search(title)]:
            continue
        if not 'wtperf' in title:
            prefix = title if prefix is None else common_prefix(prefix, title)
            suffix = title if suffix is None else common_suffix(title, suffix)
        results.append((title, ydata))

    # Process titles, eliminate common prefixes and suffixes
    if prefix or suffix:
        new_results = []
        for title, ydata in results:
            if 'wtperf' not in title:
                title = title[len(prefix):]
                if suffix:
                    title = title[:-len(suffix)]
            new_results.append((title, ydata))
        results = new_results

    # Are we just listing the results?
    if args.list:
        print 
        print "Parsed stats:"
        for title, ydata in results:
            print "  ", title
        sys.exit(0)

    output_series(results, args)

    # If the user wants the stats split up by prefix type do so.
    if args.all:
        for prefix in prefix_list:
            output_series(results, args, prefix)
        for group in groups.keys():
            output_series(results, args, group, groups[group])


if __name__ == '__main__':
    main()

