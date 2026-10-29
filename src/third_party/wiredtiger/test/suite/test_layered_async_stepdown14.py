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

# test_layered_async_stepdown14.py
#    Verify that mirrored values preserve the constituent-specific tombstone encoding while the
#    layered cursor returns the original application values.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_async_stepdown14(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_base_config = 'precise_checkpoint=true,'
    disagg_storages = gen_disagg_storages(disagg_only=True)
    encodings = [
        ('escaped', dict(encoding='true')),
        ('unescaped', dict(encoding='false')),
    ]
    write_modes = [
        ('mirrored', dict(write_mirroring=True)),
        ('ingest_only', dict(write_mirroring=False)),
    ]
    values = [
        ('collide',  dict(value=b'\x14\x14')),       # exactly the tombstone
        ('triple',   dict(value=b'\x14\x14\x14')),   # tombstone prefix + a tombstone byte
        ('mixed',    dict(value=b'\x14\x14ab')),     # tombstone prefix + non-tombstone bytes
        ('trailing', dict(value=b'\x14\x14ab\x14')), # escape and decode must not cancel
        ('normal',   dict(value=b'hello')),          # not in the namespace
    ]
    scenarios = make_scenarios(disagg_storages, encodings, write_modes, values)

    uri = f'layered:{test_name}'

    def conn_config(self):
        return self.conn_base_config + \
            f'disaggregated=(stepdown_write_mirroring={str(self.write_mirroring).lower()},' \
            f'legacy_tombstone_encoding_break_glass={self.encoding},role="leader")'

    def setUp(self):
        super().setUp()
        self.ignoreStdoutPattern(
            'stable table value in the tombstone namespace')

    # The bytes a constituent stores for a logical value: the ingest table always escapes
    # tombstone-namespace values, the stable table only while stable tombstone encoding is on.
    def stored(self, value, to_stable):
        if value[:2] == b'\x14\x14' and (not to_stable
                                         or self.encoding == 'true'):
            return value + b'\x14'
        return value

    def test_mirrored_values_round_trip(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=u')
        self.write_at(self.uri, {'base': b'base'}, 10)

        self.set_step_down_ts(20)
        self.write_at(
            self.uri, {
                'insert': self.value,
                'updated': b'plain',
                'modified': b'ab',
                'out': self.value,
                'removed': b'remove-me',
            }, 30)

        cursor = self.session.open_cursor(self.uri, None, None)

        # An update that moves the value into the tombstone namespace.
        self.session.begin_transaction()
        cursor.set_key('updated')
        cursor.set_value(self.value)
        self.assertEqual(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(31))

        # A modify that moves the value into the namespace.
        self.session.begin_transaction()
        cursor.set_key('modified')
        self.assertEqual(cursor.modify([wiredtiger.Modify(self.value, 0, 2)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(32))

        # A modify that moves the value out of the namespace: both legs must stop escaping it, or
        # the value reads back with a trailing escape byte.
        self.session.begin_transaction()
        cursor.set_key('out')
        self.assertEqual(cursor.modify([wiredtiger.Modify(b'\x14\x15', 0, 2)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(33))
        cursor.close()
        self.remove_at(self.uri, ['removed'], 34)

        expected = {
            'base': b'base',
            'insert': self.value,
            'updated': self.value,
            'modified': self.value,
            'out': b'\x14\x15' + self.value[2:],
        }
        self.assertEqual(self.read_kvs_at(self.uri, 40), expected)

        ingest_expected = {
            key: self.stored(value, False)
            for key, value in expected.items() if key != 'base'
        }
        ingest_expected['removed'] = b'\x14\x14'
        if self.stable_has_step_down_writes():
            stable_expected = {
                key: self.stored(value, True)
                for key, value in expected.items()
            }
        else:
            stable_expected = {'base': b'base'}
        self.assertEqual(self.read_kvs_at(self.stable_uri(self.uri), 40), stable_expected)
        self.assertEqual(self.read_kvs_at(self.ingest_uri(self.uri), 40), ingest_expected)
        self.complete_step_down(20)
