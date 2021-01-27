#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

# test_tiered01.py
#    Basic tiered tree test
class test_tiered01(wttest.WiredTigerTestCase):
    K = 1024
    M = 1024 * K
    G = 1024 * M
    uri = "table:test_tiered01"

    chunk_size_scenarios = wtscenario.quick_scenarios('s_chunk_size',
        [1*M,20*M,None], [0.6,0.6,0.6])
    # Occasionally add a lot of records, so that merges (and bloom) happen.
    record_count_scenarios = wtscenario.quick_scenarios(
        'nrecs', [10, 10000], [0.9, 0.1])

    config_vars = [ 'chunk_size', ]

    scenarios = wtscenario.make_scenarios(
        chunk_size_scenarios, record_count_scenarios,
        prune=100, prunelong=500)

    # Test create of an object.
    def test_tiered(self):
        self.session.create('file:first.wt', 'key_format=S')
        self.session.create('file:second.wt', 'key_format=S')
        args = 'type=tiered,key_format=S'
        args += ',tiered=(' # Start the tiered configuration options.
        args += 'tiers=("file:first.wt", "file:second.wt"),'
        # add names to args, e.g. args += ',session_max=30'
        for var in self.config_vars:
            value = getattr(self, 's_' + var)
            if value != None:
                if var == 'verbose':
                    value = '[' + str(value) + ']'
                value = {True : 'true', False : 'false'}.get(value, value)
                args += ',' + var + '=' + str(value)
        args += ')' # Close the tiered configuration option group
        self.verbose(3,
            'Test tiered with config: ' + args + ' count: ' + str(self.nrecs))
        SimpleDataSet(self, self.uri, self.nrecs, config=args).populate()

       #  self.session.drop(self.uri)

    # It is an error to configure a tiered table with no tiers
    def test_no_tiers(self):
        msg = '/tiered table must specify at least one tier/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.uri, 'type=tiered,key_format=S,tiered=(tiers=())'),
            msg)

if __name__ == '__main__':
    wttest.run()
