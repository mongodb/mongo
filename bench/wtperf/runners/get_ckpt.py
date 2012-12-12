#!/usr/bin/env python
# Generate input data to GNUplot from checkpoint information in a wtperf run

import sys

time = 0 # seconds
print "%d, %d" % (0, 0)

for line in sys.stdin:
	if line.strip().endswith('secs'):
		time += int(line.split(' ')[5])
	if line.startswith('Finished checkpoint'):
		duration = (int(line.split(' ')[3]) + 500) / 1000 # convert ms to secs
		print "%d, %d" % (time - duration, 1)
		print "%d, %d" % (time, 0)
