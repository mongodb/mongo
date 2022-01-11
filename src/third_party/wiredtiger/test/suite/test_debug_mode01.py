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

# test_debug_mode01.py
#    Test the debug mode settings. Test rollback_error in this one.
class test_debug_mode01(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled=true),debug_mode=(rollback_error=5)'
    uri = 'file:test_debug'

    entries = 22
    min_error = entries // 5

    def rollback_error(self, val, insert=True):
        keys = range(1, self.entries)
        c = self.session.open_cursor(self.uri, None)
        # We expect some operations to return an exception so we cannot
        # use the simple 'c[k] = 1'. But we must explicitly set the key
        # and value and then use the insert or update primitives.
        #
        # Look for a generic 'WT_ROLLBACK' string not the specific
        # simulated reason string.
        msg = '/WT_ROLLBACK/'
        rollback = 0
        for k in keys:
            self.session.begin_transaction()
            c.set_key(k)
            c.set_value(val)
            # Execute the insert or update. It will return true if the simulated
            # conflict exception is raised, false if no exception occurred.
            if insert:
                conflict = self.assertRaisesException(wiredtiger.WiredTigerError, \
                    lambda:c.insert(), msg, True)
            else:
                conflict = self.assertRaisesException(wiredtiger.WiredTigerError, \
                    lambda:c.update(), msg, True)

            if conflict:
                rollback += 1
                self.pr("Key: " + str(k) + " Rolled back")
                self.session.rollback_transaction()
            else:
                self.session.commit_transaction()
        c.close()
        return rollback

    def test_rollback_error(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        rollback = self.rollback_error(1)
        rollback += self.rollback_error(2, False)
        self.pr("Rollback: " + str(rollback))
        self.pr("Minimum: " + str(self.min_error))
        self.assertTrue(rollback >= self.min_error)

    def test_rollback_error_off(self):
        # The setting is added in to wiredtiger_open via the config above.
        # Test that we can properly turn the setting off via reconfigure.
        # There should then be no rollback errors.
        self.conn.reconfigure("debug_mode=(rollback_error=0)")

        self.session.create(self.uri, 'key_format=i,value_format=i')
        rollback = self.rollback_error(1)
        rollback += self.rollback_error(2)
        self.assertTrue(rollback == 0)

if __name__ == '__main__':
    wttest.run()
