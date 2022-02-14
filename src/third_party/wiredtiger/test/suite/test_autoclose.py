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

# test_autoclose
class test_autoclose(wttest.WiredTigerTestCase):
    """
    Check that when closed handles are used, there is a catchable
    error thrown, and that when a handle is closed, any subordinate
    handles are also closed.
    """
    uri = 'table:test_autoclose'

    def create_table(self):
        self.session.create(self.uri,
                            'key_format=S,value_format=S')

    def drop_table(self):
        self.dropUntilSuccess(self.session, self.uri)

    def open_cursor(self):
        cursor = self.session.open_cursor(self.uri, None, None)
        return cursor

    def test_close_cursor1(self):
        """
        Use a cursor handle after it is explicitly closed.
        """
        self.create_table()

        inscursor = self.open_cursor()
        inscursor['key1'] = 'value1'
        inscursor.close()
        self.assertRaisesHavingMessage(RuntimeError,
                                       lambda: inscursor.next(),
                                       '/wt_cursor.* is None/')
        self.drop_table()
        self.close_conn()

    def test_close_cursor2(self):
        """
        Use a cursor handle after its session is closed.
        """
        self.create_table()

        inscursor = self.open_cursor()
        inscursor['key1'] = 'value1'
        self.session.close()
        self.assertRaisesHavingMessage(RuntimeError,
                                       lambda: inscursor.next(),
                                       '/wt_cursor.* is None/')
        self.close_conn()

    def test_close_cursor3(self):
        """
        Use a cursor handle after the connection is closed.
        """
        self.create_table()

        inscursor = self.open_cursor()
        inscursor['key1'] = 'value1'
        self.close_conn()
        self.assertRaisesHavingMessage(RuntimeError,
                                       lambda: inscursor.next(),
                                       '/wt_cursor.* is None/')

    def test_close_cursor4(self):
        """
        The truncate call allows both of its cursor args
        to be null, so we don't expect null checking.
        """
        self.create_table()
        inscursor = self.open_cursor()
        inscursor['key1'] = 'value1'
        inscursor.set_key('key1')
        inscursor2 = self.session.open_cursor(None, inscursor, None)
        self.session.truncate(None, inscursor, inscursor2, '')
        inscursor.close()
        inscursor2.close()
        self.session.truncate(self.uri, None, None, '')

    def test_close_cursor5(self):
        """
        Test Cursor.compare() which should not allow a null cursor arg.
        """
        self.create_table()

        inscursor = self.open_cursor()
        inscursor['key1'] = 'value1'
        inscursor.set_key('key1')
        inscursor2 = self.session.open_cursor(None, inscursor, None)
        inscursor.compare(inscursor2)

        # Note: SWIG generates a TypeError in this case.
        # A RuntimeError to match the other cases would be better.
        inscursor2.close()
        self.assertRaises(TypeError,
                          lambda: inscursor.compare(inscursor2))

        inscursor2 = None
        self.assertRaisesHavingMessage(RuntimeError,
                                       lambda: inscursor.compare(inscursor2),
                                       '/wt_cursor.* is None/')

    def test_close_session1(self):
        """
        Use a session handle after it is explicitly closed.
        """
        self.session.close()
        self.assertRaisesHavingMessage(RuntimeError,
                                       lambda: self.create_table(),
                                       '/wt_session.* is None/')
        self.close_conn()

    def test_close_session2(self):
        """
        Use a session handle after the connection is closed.
        """
        self.close_conn()
        self.assertRaisesHavingMessage(RuntimeError,
                                       lambda: self.create_table(),
                                       '/wt_session.* is None/')

    def test_close_connection1(self):
        """
        Use a connection handle after it is closed.
        """
        conn = self.conn
        self.close_conn()
        self.assertRaisesHavingMessage(RuntimeError,
                                       lambda: conn.open_session(None),
                                       '/wt_connection.* is None/')

if __name__ == '__main__':
    wttest.run()
