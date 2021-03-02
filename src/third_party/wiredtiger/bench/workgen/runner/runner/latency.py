#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
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
# runner/latency.py
#      Utility functions for showing latency statistics
from __future__ import print_function
import sys

def _show_buckets(fh, title, mult, buckets, n):
    shown = False
    s = title + ': '
    for count in range(0, n):
        val = buckets[count]
        if val != 0:
            if shown:
                s += ','
            s += str(count*mult) + '=' + str(val)
            shown = True
    print(s, file=fh)

def _latency_preprocess(arr, merge):
    mx = 0
    cur = 0
    # SWIG arrays have a clunky interface
    for i in range(0, arr.__len__()):
        if i % merge == 0:
            cur = 0
        cur += arr[i]
        if cur > mx:
            mx = cur
    arr.height = mx

def _latency_plot(box, ch, left, width, arr, merge, scale):
    pos = 0
    for x in range(0, width):
        t = 0
        for i in range(0, merge):
            t += arr[pos]
            pos += 1
        nch = scale * t
        y = 0
        while nch > 0.0:
            box[y][left + x] = ch
            nch -= 1.0
            y += 1

def _latency_optype(fh, name, ch, t):
    if t.ops == 0:
        return
    if t.latency_ops == 0:
        print('**** ' + name + ' operations: ' + str(t.ops), file=fh)
        return
    print('**** ' + name + ' operations: ' + str(t.ops) + \
          ', latency operations: ' + str(t.latency_ops), file=fh)
    print('  avg: ' + str(t.latency/t.latency_ops) + \
          ', min: ' + str(t.min_latency) + ', max: ' + str(t.max_latency),
          file=fh)
    us = t.us()
    ms = t.ms()
    sec = t.sec()
    _latency_preprocess(us, 40)
    _latency_preprocess(ms, 40)
    _latency_preprocess(sec, 4)
    max_height = max(us.height, ms.height, sec.height)
    if max_height == 0:
        return
    height = 20    # 20 chars high
    # a list of a list of characters
    box = [list(' ' * 80) for x in range(height)]
    scale = (1.0 / (max_height + 1)) * height
    _latency_plot(box, ch, 0,  25, us, 40, scale)
    _latency_plot(box, ch, 27, 25, ms, 40, scale)
    _latency_plot(box, ch, 54, 25, sec, 4, scale)
    box.reverse()
    for line in box:
        print(''.join(line), file=fh)
    dash25 = '-' * 25
    print('  '.join([dash25] * 3), file=fh)
    print(' 0 - 999 us (40/bucket)     1 - 999 ms (40/bucket)     ' + \
          '1 - 99 sec (4/bucket)', file=fh)
    print('', file=fh)
    _show_buckets(fh, name + ' us', 1, us, 1000)
    _show_buckets(fh, name + ' ms', 1000, ms, 1000)
    _show_buckets(fh, name + ' sec', 1000000, sec, 100)
    print('', file=fh)

def workload_latency(workload, outfilename = None):
    if outfilename:
        fh = open(outfilename, 'w')
    else:
        fh = sys.stdout
    _latency_optype(fh, 'insert', 'I', workload.stats.insert)
    _latency_optype(fh, 'read', 'R', workload.stats.read)
    _latency_optype(fh, 'remove', 'X', workload.stats.remove)
    _latency_optype(fh, 'update', 'U', workload.stats.update)
    _latency_optype(fh, 'truncate', 'T', workload.stats.truncate)
    _latency_optype(fh, 'not found', 'N', workload.stats.not_found)
