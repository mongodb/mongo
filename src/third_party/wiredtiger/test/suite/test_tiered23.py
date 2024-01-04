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

import os, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtdataset import SimpleDataSet, ComplexDataSet
from wtscenario import make_scenarios

# test_tiered23.py
#    Test delays in tiered operations.
class test_tiered23(wttest.WiredTigerTestCase, TieredConfigMixin):

    # Make scenarios for different cloud service providers.
    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered23', tiered_only=True)
    scenarios = make_scenarios(storage_sources)

    uri = "table:test_tiered23"

    def conn_config(self):
        return TieredConfigMixin.conn_config(self)

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    # Override the TieredConfigMixin function to add configuration for local storage.
    # Here we can set delays and frequency of delays
    def tiered_extension_config(self):
        if self.is_local_storage:
            return 'delay_ms=130,force_delay=3'
        return ''

    # Test tiered storage with flush_tier checkpoints, these should delay.
    def test_tiered(self):
        for i in range(1, 10):
            ds = SimpleDataSet(self, self.uri, 10 * i, key_format='S')
            ds.populate()
            ds.check()
            self.session.checkpoint('flush_tier=(enabled)')
            ds.check()
