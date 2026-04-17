#!/usr/bin/env python3
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

# test_layered88.py
# Test that unsupported cursor and table operations return clear errors for layered tables:
# - Reverse collator (FIXME-WT-14738)

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered88(wttest.WiredTigerTestCase):
    disagg_storages = gen_disagg_storages('test_layered88', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'disaggregated=(role="leader")'
    uri = 'layered:test_layered88'

    # Override conn_extensions to load the reverse collator alongside the disagg page log.
    def conn_extensions(self, extlist):
        extlist.extension('collators', 'reverse')
        self.disagg_conn_extensions(extlist)

    def test_readonly(self):
        # FIXME-WT-17177: Opening a read-only connection with disagg must be rejected.
        self.close_conn()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open(self.home,
                self.extensionsConfig() + ',readonly=true,' + self.conn_config),
            '/disaggregated storage is not supported with read-only connections/')
        self.open_conn()

    def test_reverse_collator(self):
        # Opening a cursor on a layered table with a custom collator must be rejected.
        # The create succeeds (metadata is written), but opening the layered dhandle checks
        # collator support and rejects it. FIXME-WT-14738.
        self.session.create(self.uri, 'key_format=S,value_format=S,collator=reverse')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(self.uri),
            '/layered tables do not support custom collators/')
        # Drop the table so verifyLayered in tearDown doesn't try to verify an unsupported
        # configuration and fail with EINVAL.
        self.session.drop(self.uri)
