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

import os
import wttest

# test_isnew
#    database is-new method
class test_isnew(wttest.WiredTigerTestCase):

    # Test is-new of a connection.
    def test_isnew(self):
        # We just created a connection, is_new should return True.
        self.assertEquals(self.conn.is_new(), True)

        # Close and re-open the connection, is_new should return False.
        self.conn.close()
        self.conn = self.setUpConnectionOpen(".")
        self.assertEquals(self.conn.is_new(), False)

# test_gethome
#    database get-home method
class test_gethome(wttest.WiredTigerTestCase):

    # Test gethome of a connection, the initially created one is ".".
    def test_gethome_default(self):
        self.assertEquals(self.conn.get_home(), '.')

    # Create a new database directory, open it and check its name.
    def test_gethome_new(self):
        name = 'new_database'
        os.mkdir(name)
        self.conn.close()
        self.conn = self.setUpConnectionOpen(name)
        self.assertEquals(self.conn.get_home(), name)

# test_base_config
#       test base configuration file config.
class test_base_config(wttest.WiredTigerTestCase):
    def test_base_config(self):
        # We just created a database, there should be a base configuration file.
        self.assertTrue(os.path.exists("./WiredTiger.basecfg"))

        # Open up another database, configure without base configuration.
        os.mkdir("A")
        conn = self.wiredtiger_open("A", "create,config_base=false")
        self.assertFalse(os.path.exists("A/WiredTiger.basecfg"))
        conn.close()

if __name__ == '__main__':
    wttest.run()
