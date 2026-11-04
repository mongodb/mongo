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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wiredtiger import stat
from wtscenario import make_scenarios

# A leader removes through a blind_remove cursor as it does without the option, both outside and
# inside the step-down window, where writes go to the ingest table and may be mirrored to stable.
@disagg_test_class
class test_layered_cursor35(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = 'layered:' + test_name
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    windows = [
        ('leader', dict(step_down=False, write_mirroring=True)),
        ('step_down_mirrored', dict(step_down=True, write_mirroring=True)),
        ('step_down_ingest_only', dict(step_down=True, write_mirroring=False)),
    ]
    scenarios = make_scenarios(disagg_storages, windows)

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(stepdown_write_mirroring=' + \
            str(self.write_mirroring).lower() + ',role="leader")'

    def open_blind_cursor(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'v1', 'k2': 'v2'}, 10)
        if self.step_down:
            self.set_step_down_ts(20)
        return self.session.open_cursor(self.uri, None, 'blind_remove=true')

    def finish(self):
        if self.step_down:
            self.complete_step_down(20)
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

    def test_leader_remove(self):
        cursor = self.open_blind_cursor()
        self.session.begin_transaction()
        # FIXME-WT-18975: A leader must return 0 for a missing key.
        # FIXME-WT-18976: So must a leader whose step-down window writes are mirrored.
        cursor.set_key('missing')
        self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)
        cursor.set_key('k1')
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k2': 'v2'})
        self.assertEqual(self.get_stat(stat.conn.layered_curs_blind_remove_follower), 0)
        if self.step_down:
            self.assertNotIn('missing', self.read_keys_at(self.ingest_uri(self.uri), 40))
        self.finish()

    # Removing a key outside the cursor bounds fails with an invalid argument error and leaves the
    # key as it was.
    def test_leader_remove_out_of_bounds(self):
        cursor = self.open_blind_cursor()
        cursor.set_key('k2')
        cursor.bound('bound=lower')
        self.session.begin_transaction()
        cursor.set_key('k1')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.remove(),
            '/blind_remove cannot remove a key outside the cursor bounds/')
        self.session.rollback_transaction()
        cursor.close()

        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'v1', 'k2': 'v2'})
        self.finish()
