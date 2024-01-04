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

import wiredtiger, wttest, unittest
from wiredtiger import wiredtiger_strerror, WT_PREPARE_CONFLICT, WiredTigerError
from wtscenario import make_scenarios
from wtbound import bound_base

# Test prepare conflict correctness when positioning a cursor for next and prev.
#
# test_cursor_bound21.py
class test_cursor_bound21(bound_base):
    key_formats = [
        ('string', dict(key_format='S')),
        ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('bytes', dict(key_format='u')),
    ]

    scenarios = make_scenarios(key_formats)

    def test_cursor_bound_bug(self):
        uri = "table:test_cursor_bound_bug"
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        value1 = 'abc'
        value2 = 'def'

        self.session.create(uri, create_params)
        cursor = self.session.open_cursor(uri)

        session2 = self.setUpSessionOpen(self.conn)
        session2.create(uri, create_params)
        cursor2 = session2.open_cursor(uri)

        # Prepare a value.
        self.session.begin_transaction()
        cursor[self.gen_key(1)] = value1
        self.session.prepare_transaction('prepare_timestamp=5')

        # Set bounds which match the prepared key.
        cursor2.set_key(self.gen_key(1))
        cursor2.bound("bound=lower")

        # Walk the cursor next 3 times, we should only receive a prepare conflict, any other outcome
        # is a bug.
        for i in range(0 ,3):
            try:
                ret = cursor2.next()
                assert(False)
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    pass
                else:
                    raise (e)

        # Commit the prepared transaction.
        self.session.commit_transaction('commit_timestamp=6,durable_timestamp=6')

        # Walk the cursor again, ensuring it finds the lower bound key.
        assert(cursor2.next() == 0)
        if (self.key_format != 'u'):
            assert(cursor2.get_key() == self.gen_key(1))
        else:
            assert(str(cursor2.get_key().decode()) == self.gen_key(1))

        # Repeat the steps with the upper bound.
        self.session.begin_transaction()
        cursor[self.gen_key(2)] = value2
        self.session.prepare_transaction('prepare_timestamp=7')

        cursor2.reset()
        cursor2.set_key(self.gen_key(2))
        cursor2.bound("bound=upper")
        for i in range(0 ,3):
            try:
                ret = cursor2.prev()
                assert(False)
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    pass
                else:
                    raise (e)

        self.session.commit_transaction('commit_timestamp=8,durable_timestamp=8')
        assert(cursor2.prev() == 0)
        if (self.key_format != 'u'):
            assert(cursor2.get_key() == self.gen_key(2))
        else:
            assert(str(cursor2.get_key().decode()) == self.gen_key(2))

    def test_not_inclusive_bound(self):
        uri = "table:test_not_inclusive_bound"
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        value1 = 'abc'
        value2 = 'def'
        self.session.create(uri, create_params)
        cursor = self.session.open_cursor(uri)

        session2 = self.setUpSessionOpen(self.conn)
        session2.create(uri, create_params)
        cursor2 = session2.open_cursor(uri)

        # Insert a real key past key 1.
        cursor[self.gen_key(2)] = value1
        # Prepare a value.
        self.session.begin_transaction()
        cursor[self.gen_key(1)] = value1
        self.session.prepare_transaction('prepare_timestamp=5')

        # Set bounds which match the prepared key.
        cursor2.set_key(self.gen_key(1))
        cursor2.bound("bound=lower,inclusive=false")

        assert(cursor2.next() == 0)
        self.session.commit_transaction('commit_timestamp=6,durable_timestamp=6')

        self.session.begin_transaction()
        cursor[self.gen_key(3)] = value2
        cursor[self.gen_key(4)] = value2
        self.session.prepare_transaction('prepare_timestamp=7')

        cursor2.reset()
        # Set bounds which match the prepared key.
        cursor2.set_key(self.gen_key(3))
        cursor2.bound("bound=lower,inclusive=false")

        # When we walk next here we should walk to key 4 but receive a prepare conflict.
        try:
            cursor2.next()
            assert(False)
        except WiredTigerError as e:
            if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                pass
            else:
                raise (e)

        # Commit the prepared transaction, walking next should return key 4.
        self.session.commit_transaction('commit_timestamp=8,durable_timestamp=8')
        assert(cursor2.next() == 0)
        if (self.key_format != 'u'):
            assert(cursor2.get_key() == self.gen_key(4))
        else:
            assert(str(cursor2.get_key().decode()) == self.gen_key(4))

    def test_missing_bound_key_prepare(self):
        uri = "table:test_not_inclusive_bound"
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        value1 = 'abc'
        value2 = 'def'
        value3 = 'ghi'
        value4 = 'jkl'
        self.session.create(uri, create_params)
        cursor = self.session.open_cursor(uri)

        session2 = self.setUpSessionOpen(self.conn)
        session2.create(uri, create_params)
        cursor2 = session2.open_cursor(uri)

        # Insert the keys: 1, 6, 10
        cursor[self.gen_key(1)] = value1
        cursor[self.gen_key(5)] = value2
        cursor[self.gen_key(10)] = value3
        # Prepare the key: 4
        self.session.begin_transaction()
        cursor[self.gen_key(4)] = value4
        self.session.prepare_transaction('prepare_timestamp=5')

        # Set bounds between 1 and 4.
        cursor2.set_key(self.gen_key(2))
        cursor2.bound("bound=lower")

        # When we walk next here we should walk to key 4 but receive a prepare conflict. Try 3
        # times.
        for i in range(0, 3):
            try:
                ret = cursor2.next()
                assert(False)
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    pass
                else:
                    raise (e)

        # Commit the transaction.
        self.session.commit_transaction('commit_timestamp=6,durable_timestamp=6')
        assert(cursor2.next() == 0)
        if (self.key_format != 'u'):
            assert(cursor2.get_key() == self.gen_key(4))
        else:
            assert(str(cursor2.get_key().decode()) == self.gen_key(4))
