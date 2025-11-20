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

import re, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered64.py
#    Test the checksum part of the checkpoint metadata.
@disagg_test_class
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
class test_layered64(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S,type=layered'

    uri = "table:test_layered64"

    disagg_storages = gen_disagg_storages('test_layered64', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    # Test checkpoint metadata checksums.
    def test_layered64(self):
        self.conn.reconfigure('disaggregated=(role="leader")')

        self.session.create(self.uri, self.create_session_config)

        # Create tables with some data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['a'] = 'value1'
        cursor['b'] = 'value2'
        cursor['c'] = 'value3'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        # Get the checkpoint metadata string and ensure that it contains a checksum.
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.pr(f'Checkpoint metadata: {checkpoint_meta}')
        self.assertTrue('metadata_checksum=' in checkpoint_meta)

        # Extract the checksum from the checkpoint metadata.
        match = re.search(r'metadata_checksum=([0-9a-fA-F]+)', checkpoint_meta)
        checksum_hex = match.group(1)
        checksum_int = int(checksum_hex, 16)

        # Prevent the shutdown checkpoint, and restart as follower.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.restart_without_local_files(pickup_checkpoint=False)

        # Ensure that we can pick up the checkpoint without a checksum.
        checkpoint_meta_no_checksum = re.sub(r',metadata_checksum=[0-9a-fA-F]+', '', checkpoint_meta)
        self.pr(f'Checkpoint metadata without a checksum: {checkpoint_meta_no_checksum}')
        self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta_no_checksum}")')

        # Check that all the data is present.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor['a'], 'value1')
        self.assertEqual(cursor['b'], 'value2')
        self.assertEqual(cursor['c'], 'value3')
        cursor.close()

        # Restart again.
        self.restart_without_local_files(pickup_checkpoint=False)

        # Corrupt the checksum. Ensure that the follower cannot pick up the checkpoint.
        corrupted_checksum_int = checksum_int ^ 0xFF
        corrupted_checksum_hex = format(corrupted_checksum_int, 'x')
        corrupted_checkpoint_meta = checkpoint_meta.replace(f'metadata_checksum={checksum_hex}',
                                                  f'metadata_checksum={corrupted_checksum_hex}')
        self.pr(f'Corrupted checkpoint metadata: {corrupted_checkpoint_meta}')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure(
                f'disaggregated=(checkpoint_meta="{corrupted_checkpoint_meta}")'),
            "/Checkpoint metadata checksum mismatch/")
