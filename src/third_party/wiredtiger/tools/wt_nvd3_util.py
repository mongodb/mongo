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
import os, sys
from datetime import datetime

tool_dir = os.path.split(sys.argv[0])[0]
# Make sure Python finds the NVD3 in our third party directory, to
# avoid compatability issues
sys.path.append(os.path.join(tool_dir, "third_party"))

try:
    from nvd3 import lineChart
except ImportError:
    print >>sys.stderr, "Could not import nvd3. It should be installed locally."
    sys.exit(-1)

# Add a multiChart type so we can overlay line graphs
class multiChart(lineChart):
    def __init__(self, **kwargs):
        lineChart.__init__(self, **kwargs)

        # Fix the axes
        del self.axislist['yAxis']
        self.create_y_axis('yAxis1', format=kwargs.get('y_axis_format', '.02f'))
        self.create_y_axis('yAxis2', format=kwargs.get('y_axis_format', '.02f'))

TIMEFMT = "%b %d %H:%M:%S"

thisyear = datetime.today().year
def parsetime(s):
    return datetime.strptime(s, TIMEFMT).replace(year=thisyear)

