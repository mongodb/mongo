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

import wtscenario, wttest
from wtdataset import SimpleDataSet

# test_lsm01.py
#    Test LSM tree configuration options.
class test_lsm01(wttest.WiredTigerTestCase):
    K = 1024
    M = 1024 * K
    G = 1024 * M
    uri = "lsm:test_lsm01"

    chunk_size_scenarios = wtscenario.quick_scenarios('s_chunk_size',
        [1*M,20*M,None], [0.6,0.6,0.6])
    merge_max_scenarios = wtscenario.quick_scenarios('s_merge_max',
        [2,10,20,None], None)
    bloom_scenarios = wtscenario.quick_scenarios('s_bloom',
        [True,False,None], None)
    bloom_bit_scenarios = wtscenario.quick_scenarios('s_bloom_bit_count',
        [2,8,20,None], None)
    bloom_hash_scenarios = wtscenario.quick_scenarios('s_bloom_hash_count',
        [2,10,20,None], None)
    # Occasionally add a lot of records, so that merges (and bloom) happen.
    record_count_scenarios = wtscenario.quick_scenarios(
        'nrecs', [10, 10000], [0.9, 0.1])

    config_vars = [ 'chunk_size', 'merge_max', 'bloom',
                    'bloom_bit_count', 'bloom_hash_count' ]

    scenarios = wtscenario.make_scenarios(
        chunk_size_scenarios, merge_max_scenarios, bloom_scenarios,
        bloom_bit_scenarios, bloom_hash_scenarios, record_count_scenarios,
        prune=100, prunelong=500)

    # Test drop of an object.
    def test_lsm(self):
        args = 'key_format=S'
        args += ',lsm=(' # Start the LSM configuration options.
        # add names to args, e.g. args += ',session_max=30'
        for var in self.config_vars:
            value = getattr(self, 's_' + var)
            if value != None:
                if var == 'verbose':
                    value = '[' + str(value) + ']'
                if value == True:
                    value = 'true'
                if value == False:
                    value = 'false'
                args += ',' + var + '=' + str(value)
        args += ')' # Close the LSM configuration option group
        self.verbose(3,
            'Test LSM with config: ' + args + ' count: ' + str(self.nrecs))
        SimpleDataSet(self, self.uri, self.nrecs, config=args).populate()

        # TODO: Adding an explicit drop here can cause deadlocks, if a merge
        # is still happening. See issue #349.
        # self.session.drop(self.uri)

if __name__ == '__main__':
    wttest.run()
