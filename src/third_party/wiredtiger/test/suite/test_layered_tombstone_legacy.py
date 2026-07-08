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

# test_layered_tombstone_legacy.py
#   Before we reintroduced the escaping logic for the ingest tombstone value, a value equal to the
#   tombstone (0x14 0x14) could be persisted verbatim in the stable table. Write that raw value
#   straight into the stable constituent to mimic such on-disk data, and confirm the follower reads
#   it as a value, not a deletion.
#
#   Note that none of the known customers ever pass the exact 0x14 0x14 value to WT, so this is more
#   of a theoretical scenario.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_tombstone_legacy(wttest.WiredTigerTestCase):
    table = __qualname__
    conn_base_config = ',create,statistics=(all),'
    uri = f'layered:{table}'
    stable_uri = f'file:{table}.wt_stable'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def test_legacy_verbatim_tombstone_value(self):
        # A raw tombstone value on the stable table logs a warning; that is expected here.
        self.ignoreStdoutPattern('stable table value in the tombstone namespace')
        self.session.create(self.uri, 'key_format=S,value_format=u')

        follow_conn = self.wiredtiger_open('follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        follow = follow_conn.open_session('')
        follow.create(self.uri, 'key_format=S,value_format=u')

        # Write the raw tombstone bytes straight into the stable constituent, bypassing the layered
        # encoding a normal write would apply.
        c = self.session.open_cursor(self.stable_uri)
        self.session.begin_transaction()
        c['k'] = b'\x14\x14'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        c.close()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(follow_conn, self.conn)

        # The follower must return the raw value, not treat it as a delete.
        c = follow.open_cursor(self.uri)
        c.set_key('k')
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), b'\x14\x14')
        c.close()
