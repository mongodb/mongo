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

import os, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class
from wtscenario import make_scenarios

StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_layered01.py
#    Basic layered tree creation test
@disagg_test_class
class test_layered01(wttest.WiredTigerTestCase, DisaggConfigMixin):

    uri_base = "test_layered01"
    conn_config = 'verbose=[layered],disaggregated=(role="leader"),' \
                + 'disaggregated=(page_log=palm,lose_all_my_data=true)'

    uri = "layered:" + uri_base

    metadata_uris = [
            (uri, ''),
            ("file:" + uri_base + ".wt_ingest", ''),
            ("file:" + uri_base + ".wt_stable", '')
            ]

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', 'palm')

    # Check for a specific string as part of the uri's metadata.
    def check_metadata(self, uri, val_str):
        c = self.session.open_cursor('metadata:create')
        val = c[uri]
        c.close()
        self.assertTrue(val_str in val)

    # Test calling the create API for a layered table.
    def test_layered01(self):
        base_create = 'key_format=S,value_format=S,disaggregated=(page_log=palm)'

        self.pr("create layered tree")
        #conf = ',layered=true'
        conf = ''
        self.session.create(self.uri, base_create + conf)

        for u in self.metadata_uris:
            #print("Checking " + u[0])
            self.check_metadata(u[0], u[1])

