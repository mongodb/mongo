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

from suite_subprocess import suite_subprocess
import wttest
from wtdataset import SimpleDataSet, ComplexDataSet

# test_util20.py
#   Utilities: wt upgrade
class test_util20(wttest.WiredTigerTestCase, suite_subprocess):
    name = 'test_util20.a'
    create_params = 'key_format=S,value_format=S'
    num_rows = 10

    def test_upgrade_table_complex_data(self):
        # Run wt upgrade on a complex dataset and test for successful completion.
        uri = 'table:' + self.name
        ComplexDataSet(self, uri, self.num_rows).populate()
        self.runWt(['upgrade', uri])

    def test_upgrade_table_simple_data(self):
        # Run wt upgrade on a simple dataset and test for successful completion.
        uri = 'table:' + self.name
        SimpleDataSet(self, uri, self.num_rows).populate()
        self.runWt(['upgrade', uri])
