#!/usr/bin/env python
#
# Public Domain 2014-2021 MongoDB, Inc.
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

import wiredtiger, wtscenario, wttest
from wtdataset import SimpleDataSet

# test_tiered02.py
#    Test block-log-structured tree configuration options.
class test_tiered02(wttest.WiredTigerTestCase):
    K = 1024
    M = 1024 * K
    G = 1024 * M
    uri = "file:test_tiered02"

    # Occasionally add a lot of records, so that merges (and bloom) happen.
    record_count_scenarios = wtscenario.quick_scenarios(
        'nrecs', [10, 10000], [0.9, 0.1])

    scenarios = wtscenario.make_scenarios(record_count_scenarios, prune=100, prunelong=500)

    # Test drop of an object.
    def test_tiered(self):
        args = 'key_format=S,block_allocation=log-structured'
        self.verbose(3,
            'Test log-structured allocation with config: ' + args + ' count: ' + str(self.nrecs))
        #ds = SimpleDataSet(self, self.uri, self.nrecs, config=args)
        ds = SimpleDataSet(self, self.uri, 10, config=args)
        ds.populate()
        self.session.checkpoint()
        ds = SimpleDataSet(self, self.uri, 10000, config=args)
        ds.populate()

        self.reopen_conn()
        ds = SimpleDataSet(self, self.uri, 1000, config=args)
        ds.populate()

if __name__ == '__main__':
    wttest.run()
