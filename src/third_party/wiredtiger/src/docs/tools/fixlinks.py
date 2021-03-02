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
# This Python script is run as part of generating the documentation for the
# WiredTiger Python API.  It runs after doxypy, and turns method names in
# comments into references to the corresponding function in the C API.

import re, sys

def process(source):
    # Replace standard struct wrapper wording
    source = re.sub(r'(\s+#.*)Proxy of C (\w+) struct',
            lambda m: ('%sPython wrapper around C ::%s%s@copydoc ::%s' %
                      (m.group(1), m.group(2).strip('_').upper(), m.group(1),
                       m.group(2).strip('_').upper())), source)

    # Replace lowercase class names with upper case
    source = re.sub(r'(\s+#.*)__(wt_\w+)::',
        lambda m: ('%s%s::' % (m.group(1), m.group(2).upper())), source)

    # Replace "char" with "string" in comments
    source = re.sub(r'(\s+#.*)\bconst char \*', r'\1string', source)
    source = re.sub(r'(\s+#.*)\bchar const \*', r'\1string', source)
    source = re.sub(r'(\s+#.*)\bchar\b', r'\1string', source)

    # Copy documentation -- methods, then global functions
    source = re.sub(r'(\s+# )(\w+)\(self, (connection|cursor|session).*',
        lambda m: ('%s%s%s@copydoc WT_%s::%s' %
          (m.group(0), m.group(1), m.group(1), m.group(3).upper(), m.group(2))),
        source)

    source = re.sub(r'(\s+# )(wiredtiger_\w+)\(.*',
        lambda m: ('%s%s%s@copydoc ::%s' %
          (m.group(0), m.group(1), m.group(1), m.group(2))), source)

    # Replace "self, handle" with "self" -- these are typedef'ed away
    source = re.sub(r'(\s+#.*self),' +
                    r'(?:connection|cursor|session)', r'\1', source)
    return source

if __name__ == '__main__':
    sys.stdout.write(process(sys.stdin.read()))
