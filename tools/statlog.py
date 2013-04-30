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
from datetime import datetime
from subprocess import call

TIMEFMT = "%b %d %H:%M:%S"

# scale-per-second list section: BEGIN
scale_per_second_list = [
    'mapped bytes read by the block manager',
    'bytes read by the block manager',
    'bytes written by the block manager',
    'mapped blocks read by the block manager',
    'blocks read by the block manager',
    'blocks written by the block manager',
    'cache: bytes read into cache',
    'cache: bytes written from cache',
    'cache: checkpoint blocked page eviction',
    'cache: unmodified pages evicted',
    'cache: modified pages evicted',
    'cache: pages selected for eviction unable to be evicted',
    'cache: pages queued for forced eviction',
    'cache: hazard pointer blocked page eviction',
    'cache: internal pages evicted',
    'cache: internal page merge operations completed',
    'cache: internal page merge attempts that could not complete',
    'cache: internal levels merged',
    'cache: eviction server unable to reach eviction goal',
    'cache: pages walked for eviction',
    'cache: pages read into cache',
    'cache: pages written from cache',
    'pthread mutex condition wait calls',
    'cursor creation',
    'Btree cursor insert calls',
    'Btree cursor next calls',
    'Btree cursor prev calls',
    'Btree cursor remove calls',
    'Btree cursor reset calls',
    'Btree cursor search calls',
    'Btree cursor search near calls',
    'Btree cursor update calls',
    'rows merged in an LSM tree',
    'total heap memory allocations',
    'total heap memory frees',
    'total heap memory re-allocations',
    'total read I/Os',
    'page reconciliation calls',
    'page reconciliation calls for eviction',
    'reconciliation failed because an update could not be included',
    'pthread mutex shared lock read-lock calls',
    'pthread mutex shared lock write-lock calls',
    'ancient transactions',
    'transactions',
    'transaction checkpoints',
    'transactions committed',
    'transaction failures due to cache overflow',
    'transactions rolled-back',
    'total write I/Os',
    'blocks allocated',
    'block allocations requiring file extension',
    'blocks freed',
    'bloom filter false positives',
    'bloom filter hits',
    'bloom filter misses',
    'bloom filter pages evicted from cache',
    'bloom filter pages read into cache',
    'pages rewritten by compaction',
    'bytes read into cache',
    'bytes written from cache',
    'cache: checkpoint blocked page eviction',
    'unmodified pages evicted',
    'modified pages evicted',
    'data source pages selected for eviction unable to be evicted',
    'cache: pages queued for forced eviction',
    'cache: hazard pointer blocked page eviction',
    'internal pages evicted',
    'cache: internal page merge operations completed',
    'cache: internal page merge attempts that could not complete',
    'cache: internal levels merged',
    'pages read into cache',
    'overflow pages read into cache',
    'pages written from cache',
    'raw compression call failed, no additional data available',
    'raw compression call failed, additional data available',
    'raw compression call succeeded',
    'compressed pages read',
    'compressed pages written',
    'page written failed to compress',
    'page written was too small to compress',
    'cursor creation',
    'cursor insert calls',
    'bulk-loaded cursor-insert calls',
    'cursor-insert key and value bytes inserted',
    'cursor next calls',
    'cursor prev calls',
    'cursor remove calls',
    'cursor-remove key bytes removed',
    'cursor reset calls',
    'cursor search calls',
    'cursor search near calls',
    'cursor update calls',
    'cursor-update value bytes updated',
    'queries that could have benefited from a Bloom filter that did not exist',
    'reconciliation dictionary matches',
    'reconciliation overflow keys written',
    'reconciliation overflow values written',
    'reconciliation pages deleted',
    'reconciliation pages merged',
    'page reconciliation calls',
    'page reconciliation calls for eviction',
    'reconciliation failed because an update could not be included',
    'reconciliation internal pages split',
    'reconciliation leaf pages split',
    'object compaction',
    'update conflicts',
    'write generation conflicts',
]
# scale-per-second list section: END

# Plot a set of entries for a title.
def plot(title, values, num):
    # Ignore entries where the value never changes.
    skip = True
    t0, v0 = values[0]
    for t, v in values:
        if v != v0:
            skip = False
            break
    if skip:
        print '\tskipping ' + title
        return

    print 'building ' + title

    ylabel = 'Value'
    if title.split(' ', 1)[1] in scale_per_second_list:
        t1, v1 = values[1]
        seconds = (datetime.strptime(t1, TIMEFMT) -
            datetime.strptime(t0, TIMEFMT)).seconds
        ylabel += ' per second'
    else:
        seconds = 1

    # Write the raw data into a file for processing.
    of = open("reports/raw/report.%s.raw" % num, "w")
    for t, v in sorted(values):
        print >>of, "%s %g" % (t, float(v) / seconds)
    of.close()

    # Write a command file for gnuplot.
    of = open("gnuplot.cmd", "w")
    of.write('''
set terminal png nocrop
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
for rno, items in enumerate(sorted(d.iteritems()), 1):
    plot(items[0], items[1], "%03d" % rno)
