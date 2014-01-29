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

import csv, os, sys
from subprocess import call
# Python script to read wtperf monitor output and create a performance
# graph.

TIMEFMT = "%b %d %H:%M:%S"

def process_monitor(fname, ckptlist):
    # Read the monitor file and figure out when a checkpoint was running.
    in_ckpt = 'N'

    ckptlist=[]
    with open(fname, 'r') as csvfile:
        reader = csv.reader(csvfile)
        for row in reader:
            if row[4] != in_ckpt:
                ckptlist.append(row[0])
                in_ckpt = row[4]
    if in_ckpt == 'Y':
        ckptlist.append(row[0])

    # Graph time vs. read, insert and update operations per second.
    gcmd = "gnuplot.mon.cmd"
    of = open(gcmd, "w")
    of.write('''
set autoscale
set datafile sep ','
set grid
set style data lines
set terminal png nocrop size 800,600
set timefmt "%(TIMEFMT)s"
set title "read, insert and update operations per second"
set format x "%(TIMEFMT)s"
set xlabel "Time"
set xtics rotate by -45
set xdata time
set ylabel "Operations per second (thousands)"
set yrange [0:]\n''' % {
    'TIMEFMT' : TIMEFMT
        })
    it = iter(ckptlist)
    for start, stop in zip(it, it):
        of.write('set object rectangle from first \'' + start +\
            '\', graph 0 ' + ' to first \'' + stop +\
            '\', graph 1 fc rgb "gray" back\n') 
    of.write('set output "' + fname + '.png"\n')
    of.write('plot "' + fname + '" using 1:($2/1000) title "Reads", "' +\
        fname + '" using 1:($3/1000) title "Inserts", "' +\
        fname + '" using 1:($4/1000) title "Updates"\n')
    of.close()
    call(["gnuplot", gcmd])
    os.remove(gcmd)

# Graph time vs. average, minimium, maximum latency for an operation.
def plot_latency_operation(name, fname, sfx, ckptlist, col_avg, col_min, col_max):
    gcmd = "gnuplot." + name + ".l1.cmd"
    of = open(gcmd, "w")
    of.write('''
set autoscale
set datafile sep ','
set grid
set style data lines
set terminal png nocrop size 800,600
set timefmt "%(TIMEFMT)s"
set title "%(NAME)s: average, minimum and maximum latency"
set format x "%(TIMEFMT)s"
set xlabel "Time"
set xtics rotate by -45
set xdata time
set ylabel "Latency (us)"
set logscale y
set yrange [1:]\n''' % {
    'NAME' : name,
    'TIMEFMT' : TIMEFMT
    })
    it = iter(ckptlist)
    for start, stop in zip(it, it):
        of.write('set object rectangle from first \'' + start +\
            '\', graph 0 ' + ' to first \'' + stop +\
            '\', graph 1 fc rgb "gray" back\n')
    ofname = name + sfx + '.latency1.png'
    of.write('set output "' + ofname + '"\n')
    of.write('plot "' + fname + '" using 1:($' + repr(col_avg) +\
        ') title "Average Latency", "' + fname +'" using 1:($' +\
        repr(col_min) + ') title "Minimum Latency", "' +\
        fname + '" using 1:($' + repr(col_max) + ') title "Maximum Latency"\n')
    of.close()
    call(["gnuplot", gcmd])
    os.remove(gcmd)


# Graph latency vs. % operations
def plot_latency_percent(name, sfx, ckptlist):
    gcmd = "gnuplot." + name + ".l2.cmd"
    of = open(gcmd, "w")
    of.write('''
set autoscale
set datafile sep ','
set grid
set style data points
set terminal png nocrop size 800,600\n''')
    of.write('set title "' + name + ': latency distribution"\n')
    of.write('''
set xlabel "Latency (us)"
set xrange [1:]
set xtics rotate by -45
set logscale x
set ylabel "%% operations"
set yrange [0:]\n''')
    ofname = name + sfx + '.latency2.png'
    of.write('set output "' + ofname + '"\n')
    of.write('plot "latency.' + name + sfx +\
        '" using (($2 * 100)/$4) title "' + name + '"\n')
    of.close()
    call(["gnuplot", gcmd])
    os.remove(gcmd)


# Graph latency vs. % operations (cumulative)
def plot_latency_cumulative_percent(name, sfx, ckptlist):
    # Latency plot: cumulative operations vs. latency
    gcmd = "gnuplot." + name + ".l3.cmd"
    of = open(gcmd, "w")
    of.write('''
set autoscale
set datafile sep ','
set grid
set style data lines
set terminal png nocrop size 800,600
set title "%(NAME)s: cumulative latency distribution"
set xlabel "Latency (us)"
set xrange [1:]
set xtics rotate by -45
set logscale x
set ylabel "%% operations"
set yrange [0:]\n''' % {
        'NAME' : name
        })
    ofname = name + sfx + '.latency3.png'
    of.write('set output "' + ofname + '"\n')
    of.write('plot "latency.' + name + sfx + \
        '" using 1:(($3 * 100)/$4) title "' + name + '"\n')
    of.close()
    call(["gnuplot", gcmd])
    os.remove(gcmd)

def process_file(fname):
    ckptlist=[]
    process_monitor(fname, ckptlist)
    
    # This assumes the monitor file has the string "monitor"
    # and any other (optional) characters in the filename are a suffix.
    sfx=fname.replace('monitor','')

    column = 6              # average, minimum, maximum start in column 6

    # NOTE: The operations below must be in this exact order to match
    # the operation latency output in the monitor file.
    for op in ['read', 'insert', 'update']:
        plot_latency_operation(
            op, fname, sfx, ckptlist, column, column + 1, column + 2)
        column = column + 3
        plot_latency_percent(op, sfx, ckptlist)
        plot_latency_cumulative_percent(op, sfx, ckptlist)

def main():
    # This program takes a list of monitor files generated by
    # wtperf.  If no args are given, it looks for a single file
    # named 'monitor'.
    numargs = len(sys.argv)
    if numargs < 2:
        process_file('monitor')
    else:
        d = 1
        while d < numargs:
            process_file(sys.argv[d])
            d += 1

if __name__ == '__main__':
    main()
