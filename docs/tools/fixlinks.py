#!/usr/bin/env python

# Copyright (c) 2011 WiredTiger, Inc.
# All rights reserved.

# This Python script is run as part of generating the documentation for the
# WiredTiger Python API.  It runs after doxypy, and turns method names in
# comments into references to the corresponding function in the C API.

import re, sys

def process(source):
    # Replace standard struct wrapper wording
	source = re.sub(r'(\s+#.*)Proxy of C (\w+) struct',
			lambda m: ('%sPython wrapper around C ::%s%s@copydoc ::%s' % (m.group(1), m.group(2).upper(), m.group(1), m.group(2).upper())), source)

    # Replace lowercase class names with upper case
	source = re.sub(r'(\s+#.*)(wt_\w+)::',
		lambda m: ('%s%s::' % (m.group(1), m.group(2).upper())), source)

    # Replace "char" with "string" in comments
	while True:
		newsource = re.sub(r'(\s+#.*)\bchar\b', r'\1string', source)
		if newsource == source:
			break
		source = newsource

    # Copy documentation
	source = re.sub(r'(\s+# )(\w+)\(self, (connection|cursor|session).*',
		lambda m: ('%s%s%s@copydoc WT_%s::%s' %
		  (m.group(0), m.group(1), m.group(1), m.group(3).upper(), m.group(2))),
		source)

    # Replace "self, handle" with "self" -- these are typedef'ed away
	source = re.sub(r'(\s+#.*self), (?:connection|cursor|session)', r'\1', source)
	return source

if __name__ == '__main__':
	sys.stdout.write(process(sys.stdin.read()))
