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
from wtscenario import make_scenarios
from wtbound import bound_base

# test_cursor_bound16.py
#    Testing basic scenarios with cursor bound functionality on dump cursors.
class test_cursor_bound16(bound_base):
    file_name = 'test_cursor_bound16'
    key_format = "S"
    value_format = "S"
    start_key = None
    end_key = None

    types = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:')),
    ]

    dump_options = [
        ('dump_print', dict(dumpopt='print')),
        ('dump_hex', dict(dumpopt='hex')),
        # FIXME-WT-9986: Re-enable dump_json after fixing the JSON cursor bug
        # triggered by allocator changes.
        # ('dump_json', dict(dumpopt='json')),
    ]

    scenarios = make_scenarios(dump_options, types)

    def create_session_and_cursor(self):
        uri = self.uri + self.file_name
        create_params = 'value_format={},key_format={}'.format(self.value_format, self.key_format)
        self.session.create(uri, create_params)

        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()

        for i in range(20, 80):
            cursor[str(i)] = self.gen_val("value" + str(i))
        self.session.commit_transaction()

        self.start_key = self.gen_dump_key(20)
        self.end_key = self.gen_dump_key(80)

    def gen_dump_key(self, key):
        if (self.dumpopt == "hex"):
            return str(key).encode().hex() + "00"
        elif (self.dumpopt == "print"):
            return str(key) + "\\00"
        elif (self.dumpopt == 'json'):
            return '"key0" : "{0}"'.format(str(key))
        
        return None

    def test_dump_cursor(self):
        self.create_session_and_cursor()

        dumpopt = "dump={}".format(self.dumpopt)
        dumpcurs = self.session.open_cursor(self.uri + self.file_name,
                                                None, dumpopt)

        # Test bound api: Set bounds at lower key 30 and upper key at 50.
        self.set_bounds(dumpcurs, self.gen_dump_key(30), "lower")
        self.set_bounds(dumpcurs, self.gen_dump_key(50), "upper")
        self.cursor_traversal_bound(dumpcurs, self.gen_dump_key(30), self.gen_dump_key(50), True, 21)
        self.cursor_traversal_bound(dumpcurs, self.gen_dump_key(30), self.gen_dump_key(50), False, 21)
        
        # Test bound api: Test basic search near scenarios.
        dumpcurs.set_key(self.gen_dump_key(20))
        self.assertEqual(dumpcurs.search_near(), 1)
        self.assertEqual(dumpcurs.get_key(), self.gen_dump_key(30))

        dumpcurs.set_key(self.gen_dump_key(40))
        self.assertEqual(dumpcurs.search_near(), 0)
        self.assertEqual(dumpcurs.get_key(), self.gen_dump_key(40))

        dumpcurs.set_key(self.gen_dump_key(60))
        self.assertEqual(dumpcurs.search_near(), -1)
        self.assertEqual(dumpcurs.get_key(), self.gen_dump_key(50))

        # Test bound api: Test basic search scenarios.
        dumpcurs.set_key(self.gen_dump_key(20))
        self.assertEqual(dumpcurs.search(), wiredtiger.WT_NOTFOUND)

        dumpcurs.set_key(self.gen_dump_key(40))
        self.assertEqual(dumpcurs.search(), 0)

        dumpcurs.set_key(self.gen_dump_key(60))
        self.assertEqual(dumpcurs.search(), wiredtiger.WT_NOTFOUND)

        # Test bound api: Test that cursor resets the bounds.
        self.assertEqual(dumpcurs.reset(), 0)
        self.cursor_traversal_bound(dumpcurs, self.start_key, self.end_key, True, 60)
        self.cursor_traversal_bound(dumpcurs, self.start_key, self.end_key, False, 60)

        # Test bound api: Test that cursor action clear works and clears the bounds.
        self.set_bounds(dumpcurs, self.gen_dump_key(30), "lower")
        self.set_bounds(dumpcurs, self.gen_dump_key(50), "upper")
        self.assertEqual(dumpcurs.bound("action=clear"), 0)
        self.cursor_traversal_bound(dumpcurs, self.start_key, self.end_key, True, 60)
        self.cursor_traversal_bound(dumpcurs, self.start_key, self.end_key, False, 60)
        
        dumpcurs.close()

if __name__ == '__main__':
    wttest.run()
