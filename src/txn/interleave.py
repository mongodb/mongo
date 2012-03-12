#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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

# identical transactions
t1 = ["r1(x)", "w1(x)"]
t2 = ["r2(x)", "w2(x)"]
t3 = ["r3(x)", "w3(x)"]
t4 = ["r4(x)", "w4(x)"]

def interleave(T1, T2):
    """Given lists of operations as input, return all possible interleavings"""
    if not T1:
        return [T2]
    elif not T2:
        return [T1]
    else:
        return [T1[0:1] + l for l in interleave(T1[1:], T2)] + [T2[0:1] + l for l in interleave(T1, T2[1:])]

for l1 in interleave(t1, t2):
 for l2 in interleave(l1, t3):
  for l in interleave(l2, t4):
    # timestamps of item x
    readts = 0
    writets = 0
    skip = False
    failure = ''

    for op in l:
        ts = int(op[1])
        # Check whether the operation is valid:
        if op[0] == 'r':
            if writets < ts and readts < ts:
                readts = ts
        elif op[0] == 'w':
            if writets > ts or readts > ts:
                if ts <= 2 and l.index("r" + str(ts+2) + "(x)") < l.index("w" + str(ts+1) + "(x)"):
                    skip = True
                    break
                failure += ' ' + op
        elif op[0] == 'c':
            pass
    if skip:
        continue
    elif failure:
        print '%s: failed at%s' % (' '.join(l), failure)
    else:
        print '%s: passed' % (' '.join(l),)
