#!/usr/bin/python
#

import sys

for line in sys.stdin:
    # If line is a 'noisy' warning, don't print it or the following two lines.
    if ('warning: section' in line and 'is deprecated' in line
            or 'note: change section name to' in line):
        next(sys.stdin)
        next(sys.stdin)
    else:
        sys.stdout.write(line)
        sys.stdout.flush()