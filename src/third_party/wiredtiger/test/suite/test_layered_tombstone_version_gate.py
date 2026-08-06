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

# test_layered_tombstone_version_gate.py
#   Stable data written unescaped raises the checkpoint's minimum reader version: a new database
#   stores values unescaped and records compatible_version=2 (a node that still strips the escape
#   byte cannot read it), while an escaped checkpoint (the legacy format, fabricated here with the
#   break-glass override) stays compatible with every reader at compatible_version=1. A reader
#   whose maximum version is below the checkpoint's compatible_version must refuse the pickup. This
#   build's maximum reader version is WT_DISAGG_CHECKPOINT_META_VERSION (2), so to stand in for an
#   older reader we rewrite the fetched metadata to demand version 3 and confirm the pickup is
#   rejected with a not-supported error.

import re
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_tombstone_version_gate(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'

    # The stable tombstone encoding mode determines the checkpoint's compatible_version: the
    # legacy escaped mode is forced with the break-glass override, the unescaped mode is what a new
    # database adopts automatically.
    encodings = [
        ('escaped',   dict(encoding_config='legacy_tombstone_encoding_break_glass=true,',
                           compatible_version=1)),
        ('unescaped', dict(encoding_config='', compatible_version=2)),
    ]
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, encodings)

    def conn_config(self):
        return self.extensionsConfig() + ',create,' + self.conn_base_config + \
            f'disaggregated=({self.encoding_config}role="leader")'

    def setUp(self):
        super().setUp()
        self.ignoreStdoutPattern('stable table value in the tombstone namespace')

    # Write a colliding value and checkpoint, recording the versions in the completed-checkpoint
    # metadata.
    def leader_checkpoint(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.session.create(self.uri, 'key_format=i,value_format=u')
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c[1] = b'\x14\x14ab'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

    def test_reader_version_gate(self):
        self.leader_checkpoint()

        # The compatible_version records the encoding mode: unescaped raises the minimum reader
        # version to 2, escaped stays at 1.
        meta = self.disagg_get_complete_checkpoint_meta()
        self.assertIn(f'compatible_version={self.compatible_version}', meta)

        # Rewrite the compatible_version above this build's maximum reader version (2) to fake an
        # older reader without a second binary; leave version untouched so the not-supported check
        # (compatible_version > reader max) is what fires, not the illegal-version check.
        gated = re.sub(r'compatible_version=\d+', 'compatible_version=3', meta)
        self.assertIn('compatible_version=3', gated)

        # A follower adopts the checkpoint's own encoding mode and would otherwise pick it up
        # cleanly; the version gate is the only reason the pickup is refused.
        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config +
            'disaggregated=(role="follower")')
        with self.expectedStderrPattern('requires reader version'):
            with self.assertRaises(wiredtiger.WiredTigerError):
                conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{gated}")')

        # The unmodified metadata still picks up, confirming the checkpoint itself is readable and
        # only the forged compatible_version was rejected.
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{meta}")')
        s = conn_follow.open_session()
        rc = s.open_cursor(self.uri)
        s.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        rc.set_key(1)
        self.assertEqual(rc.search(), 0)
        self.assertEqual(rc.get_value(), b'\x14\x14ab')
        s.rollback_transaction()
        rc.close()
        s.close()
        conn_follow.close()
