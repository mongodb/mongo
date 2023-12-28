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
#
# test_search_near05.py
#       Search_near with a key past the end.

import wttest
from wtscenario import make_scenarios

class test_search_near05(wttest.WiredTigerTestCase):
    uri = 'file:test_search_near05'

    key_format_values = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('var', dict(key_format='r', value_format='I')),
        ('row', dict(key_format='Q', value_format='I')),
    ]

    ops = [
        ('update', dict(delete=False)),
        ('delete', dict(delete=True)),
    ]

    scenarios = make_scenarios(key_format_values, ops)

    def evict(self, value):
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        for i in range(1, 1001):
            v = evict_cursor[i]
            self.assertEqual(v, value)
            self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

    def test_implicit_record_cursor_insert_next(self):
        self.session.create(self.uri, 'key_format={},value_format={}'.format(self.key_format, self.value_format))
        cursor = self.session.open_cursor(self.uri)
        value1 = 1
        value2 = 2
        for i in range(1, 1001):
            cursor[i] = value1

        # Do a checkpoint to write everything to the disk image
        self.session.checkpoint()
        # Evict the data
        self.evict(value1)

        # Update or delete the last key
        if self.delete:
            self.session.begin_transaction()
            cursor.set_key(1000)
            cursor.remove()
            self.session.commit_transaction()
        else:
            cursor[1000] = value2

        self.session.begin_transaction()
        cursor.set_key(1100)
        cursor.search_near()

        if self.delete:
            if self.value_format == "8t":
                self.assertEqual(cursor.get_key(), 1000)
                self.assertEqual(cursor.get_value(), 0)
            else:
                self.assertEqual(cursor.get_key(), 999)
                self.assertEqual(cursor.get_value(), value1)
        else:
            self.assertEqual(cursor.get_key(), 1000)
            self.assertEqual(cursor.get_value(), value2)

if __name__ == '__main__':
    wttest.run()
