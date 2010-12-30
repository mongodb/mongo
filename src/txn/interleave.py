#!/usr/bin/env python

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
