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

import wiredtiger, wttest
from wtscenario import make_scenarios, filter_scenarios

# test_rollback_to_stable25.py
# Check various scenarios relating to RLE cells in column-store.
#
# We write at three different timestamps:
#    10 - aaaaaa or none
#    20 - bbbbbb or delete or none
#    30 - cccccc or delete or none
#
# and we evict to push things to disk after any of these,
# and we roll back to either 15 or 25.
#
# The writes can be either uniform, heterogeneous, first key, middle key, or last key.
#
# We do this with a group of 5 keys 2..6. Keys 1 and 6 are written with zzzzzz at
# timestamp 5 and evicted to ensure that the group of keys we're using is isolated
# from other unused keys.
#
# This generates a lot of cases, but we filter pointless combinations and they run fast.

# Put these bits outside the class definition so they can be referred to both in class
# instances and in the scenario setup logic, which doesn't have a class instance yet.

my_rle_size = 5

def keys_of_write(write):
    if write == 'u' or write == 'h':
        return range(2, 2 + my_rle_size)
    elif write == 'f':
        return [2]
    elif write == 'm':
        return [2 + my_rle_size // 2]
    else:
        return [2 + my_rle_size - 1]

class test_rollback_to_stable25(wttest.WiredTigerTestCase):
    conn_config = 'in_memory=false'

    write_10_values = [
        ('10u', dict(write_10='u')),
        ('10h', dict(write_10='h')),
        ('10f', dict(write_10='f')),
        ('10m', dict(write_10='m')),
        ('10l', dict(write_10='l')),
    ]
    type_10_values = [
        ('nil', dict(type_10=None)),
        ('upd', dict(type_10='upd')),
    ]

    write_20_values = [
        ('20u', dict(write_20='u')),
        ('20h', dict(write_20='h')),
        ('20f', dict(write_20='f')),
        ('20m', dict(write_20='m')),
        ('20l', dict(write_20='l')),
    ]
    type_20_values = [
        ('nil', dict(type_20=None)),
        ('upd', dict(type_20='upd')),
        ('del', dict(type_20='del')),
    ]

    write_30_values = [
        ('30u', dict(write_30='u')),
        ('30h', dict(write_30='h')),
        ('30f', dict(write_30='f')),
        ('30m', dict(write_30='m')),
        ('30l', dict(write_30='l')),
    ]
    type_30_values = [
        ('nil', dict(type_30=None)),
        ('upd', dict(type_30='upd')),
        ('del', dict(type_30='del')),
    ]

    evict_time_values = [
        ('chk10', dict(evict_time=10)),
        ('chk20', dict(evict_time=20)),
        ('chk30', dict(evict_time=30)),
    ]

    rollback_time_values = [
        ('roll15', dict(rollback_time=15)),
        ('roll25', dict(rollback_time=25)),
    ]

    def is_meaningful(name, vals):
        # The last write at evict time should be uniform, to get an RLE cell.
        if vals['evict_time'] == 10 and vals['write_10'] != 'u':
            return False
        if vals['evict_time'] == 20 and vals['write_20'] != 'u':
            return False
        if vals['evict_time'] == 30 and vals['write_30'] != 'u':
            return False
        # If the type is nil, the value must be uniform.
        if vals['type_10'] is None and vals['write_10'] != 'u':
            return False
        if vals['type_20'] is None and vals['write_20'] != 'u':
            return False
        if vals['type_30'] is None and vals['write_30'] != 'u':
            return False
        # Similarly, delete and heterogeneous doesn't make sense.
        if vals['type_10'] == 'del' and vals['write_10'] == 'h':
            return False
        if vals['type_20'] == 'del' and vals['write_20'] == 'h':
            return False
        if vals['type_20'] == 'del' and vals['write_30'] == 'h':
            return False
        # Both 10 and 20 shouldn't be nil. That's equivalent to 10 and 30 being nil.
        if vals['type_10'] is None and vals['type_20'] is None:
            return False

        # Avoid cases that delete nonexistent values.
        def deletes_nonexistent():
            present = {}
            for k in range(2, 2 + my_rle_size):
                present[k] = False
            def adjust(ty, write):
                if ty is None:
                    return
                for k in keys_of_write(write):
                    if ty == 'upd':
                         present[k] = True
                    elif ty == 'del':
                        if present[k]:
                            present[k] = False
                        else:
                            raise KeyError

            adjust(vals['type_10'], vals['write_10'])
            adjust(vals['type_20'], vals['write_20'])
            adjust(vals['type_30'], vals['write_30'])
        try:
            deletes_nonexistent()
        except KeyError:
            return False
        return True

    scenarios = filter_scenarios(make_scenarios(write_10_values, type_10_values,
                                                write_20_values, type_20_values,
                                                write_30_values, type_30_values,
                                                evict_time_values,
                                                rollback_time_values),
                                 is_meaningful)

    value_z = "zzzzz" * 10

    def writes(self, uri, s, expected, ty, write, value, ts):
        if ty is None:
             # do nothing at all
             return
        cursor = s.open_cursor(uri)
        s.begin_transaction()
        for k in keys_of_write(write):
            if ty == 'upd':
                myval = value + str(k) if write == 'h' else value
                cursor[k] = myval
                expected[k] = myval
            else:
                cursor.set_key(k)
                cursor.remove()
                del expected[k]
        s.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def evict(self, uri, s):
        # Evict the page to force reconciliation.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")
        s.begin_transaction()
        # Search the key to evict it. Use both bookends.
        v = evict_cursor[1]
        self.assertEqual(v, self. value_z)
        v = evict_cursor[2 + my_rle_size]
        self.assertEqual(v, self. value_z)
        self.assertEqual(evict_cursor.reset(), 0)
        s.rollback_transaction()
        evict_cursor.close()

    def check(self, uri, s, ts, expected):
        cursor = s.open_cursor(uri)
        s.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        # endpoints should still be in place
        self.assertEqual(cursor[1], self.value_z)
        self.assertEqual(cursor[2 + my_rle_size], self.value_z)

        for k in range(2, 2 + my_rle_size):
            if k in expected:
                self.assertEqual(cursor[k], expected[k])
            else:
                cursor.set_key(k)
                r = cursor.search()
                self.assertEqual(r, wiredtiger.WT_NOTFOUND)
        s.rollback_transaction()
        cursor.close()

    def test_rollback_to_stable25(self):
        # Create a table without logging.
        uri = "table:rollback_to_stable25"
        self.session.create(uri, 'key_format=r,value_format=S')

        # Pin oldest timestamp to 2.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(2))

        # Start stable timestamp at 2.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))

        value_a = "aaaaa" * 10
        value_b = "bbbbb" * 10
        value_c = "ccccc" * 10

        s = self.conn.open_session()

        # Write the endpoints at time 5.
        cursor = s.open_cursor(uri)
        s.begin_transaction()
        cursor[1] = self.value_z
        cursor[2 + my_rle_size] = self.value_z
        s.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.evict(uri, s)
        cursor.close()

        # Do writes at time 10.
        expected = {}
        self.writes(uri, s, expected, self.type_10, self.write_10, value_a, 10)
        expected10 = expected.copy()

        # Evict at time 10 if requested.
        if self.evict_time == 10:
            self.evict(uri, s)

        # Do more writes at time 20.
        self.writes(uri, s, expected, self.type_20, self.write_20, value_b, 20)
        expected20 = expected.copy()

        # Evict at time 20 if requested.
        if self.evict_time == 20:
            self.evict(uri, s)

        # Do still more writes at time 30.
        self.writes(uri, s, expected, self.type_30, self.write_30, value_c, 30)
        expected30 = expected.copy()

        # Evict at time 30 if requested.
        if self.evict_time == 30:
            self.evict(uri, s)

        # Now roll back.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.rollback_time))
        self.conn.rollback_to_stable()

        if self.rollback_time < 20:
            expected20 = expected10
            expected30 = expected10
        elif self.rollback_time < 30:
            expected30 = expected20

        # Now make sure we see what we expect.
        self.check(uri, s, 10, expected10)
        self.check(uri, s, 20, expected20)
        self.check(uri, s, 30, expected30)
