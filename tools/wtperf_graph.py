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

import csv, os
from subprocess import call
# Python script to read wtperf monitor output and create a performance
# graph.

TIMEFMT = "%b %d %H:%M:%S"

# Read the monitor file and figure out when a checkpoint was running.
in_ckpt = 'N'
ckptlist=[]
with open('monitor', 'r') as csvfile:
    reader = csv.reader(csvfile)
    for row in reader:
        if row[4] != in_ckpt:
            ckptlist.append(row[0])
            in_ckpt = row[4]
if in_ckpt == 'Y':
    ckptlist.append(row[0])


# Graph time vs. read, insert and update operations per second.
of = open("gnuplot.cmd", "w")
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
set ylabel "Operations per second (hundreds)"
set yrange [0:]\n''' % {
    'TIMEFMT' : TIMEFMT
    })
it = iter(ckptlist)
for start, stop in zip(it, it):
    of.write('set object rectangle from first \'' + start +\
        '\', graph 0 ' + ' to first \'' + stop +\
        '\', graph 1 fc rgb "gray" back\n')
of.write('''
set output 'monitor.png'
plot "monitor" using 1:($2/100) title "Reads", "monitor" using 1:($3/100) title "Updates", "monitor" using 1:($4/100) title "Inserts"\n''')
of.close()
call(["gnuplot", "gnuplot.cmd"])
os.remove("gnuplot.cmd")


# Graph time vs. average, minimium, maximum latency for an operation.
def plot_latency_operation(name, col_avg, col_min, col_max):
    of = open("gnuplot.cmd", "w")
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
    of.write('''
set output '%(NAME)s.latency1.png'
plot "monitor" using 1:($%(COL_AVG)d / 1000) title "Average Latency", "monitor" using 1:($%(COL_MIN)d / 1000) title "Minimum Latency", "monitor" using 1:($%(COL_MAX)d / 1000) title "Maximum Latency"\n''' % {
    'NAME' : name,
    'COL_AVG' : col_avg,
    'COL_MIN' : col_min,
    'COL_MAX' : col_max
    })
    of.close()
    call(["gnuplot", "gnuplot.cmd"])
    os.remove("gnuplot.cmd")


# Graph latency vs. % operations
def plot_latency_percent(name):
    of = open("gnuplot.cmd", "w")
    of.write('''
set autoscale
set datafile sep ','
set grid
set style data points
set terminal png nocrop size 800,600
set title "%(NAME)s: latency distribution"
set xlabel "Latency (us)"
set xrange [1:]
set xtics rotate by -45
set logscale x
set ylabel "%% operations"
set yrange [0:]
set output '%(NAME)s.latency2.png'
plot "latency.%(NAME)s" using (($2 * 100)/$4) title "%(NAME)s"\n''' % {
        'NAME' : name
        })
    of.close()
    call(["gnuplot", "gnuplot.cmd"])
    os.remove("gnuplot.cmd")


# Graph latency vs. % operations (cumulative)
def plot_latency_cumulative_percent(name):
    # Latency plot: cumulative operations vs. latency
    of = open("gnuplot.cmd", "w")
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
set yrange [0:]
set output '%(NAME)s.latency3.png'
plot "latency.%(NAME)s" using 1:(($3 * 100)/$4) title "%(NAME)s"\n''' % {
        'NAME' : name
        })
    of.close()
    call(["gnuplot", "gnuplot.cmd"])
    os.remove("gnuplot.cmd")


column = 6              # average, minimum, maximum start in column 6
for op in ['read', 'insert', 'update']:
    plot_latency_operation(op, column, column + 1, column + 2)
    column = column + 3
    plot_latency_percent(op)
    plot_latency_cumulative_percent(op)
