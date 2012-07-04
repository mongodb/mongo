#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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
# test_util13.py
# 	Utilities: wt backup
#

import string, os
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from helper import compareFiles

# Test backup (both backup cursors and the wt backup command).
class test_util13(wttest.WiredTigerTestCase, suite_subprocess):
    namepfx = 'test_util13.'

    objs=4                      # Number of objects
    dir='backup.dir'            # Backup directory name

    # Populate a set of objects.
    def populate(self):
        params = 'key_format=S,value_format=S'
        for type in [ 'table:', 'file:' ]:
            for obj in range(1, self.objs):
                name = type + self.namepfx + str(obj)
                self.session.create(name, params)
                cursor = self.session.open_cursor(name, None, None)
                for i in range(1, 100):
                    cursor.set_key('KEY' + str(i))
                    cursor.set_value('VALUE' + str(i))
                    cursor.insert()
                cursor.close()

    # Dump and compare the original and backed-up file
    def compare(self, name):
        self.runWt(['dump', name], outfilename='orig')
        self.runWt(['-h', self.dir, 'dump', name], outfilename='backup')
        compareFiles('orig', 'backup')

    # Test backup of a database in a 'wt' process.
    def test_backup_database(self):
        self.populate()
        os.mkdir(self.dir)
        self.runWt(['backup', self.dir])
        for n in range(1, self.objs):
            self.compare('table:' + self.namepfx + str(n))

    # Test backup of a table in a 'wt' process.
    def test_backup_table(self):
        self.populate()
        os.mkdir(self.dir)
        for i in range(1, 2):
            self.runWt(
                ['backup', '-t', 'table:' + self.namepfx + '1', self.dir])
        for n in range(1, 2):
            self.compare('table:' + self.namepfx + str(n))

        # The files shouldn't be there.
        conn = wiredtiger.wiredtiger_open(self.dir)
        session = conn.open_session()
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            session.open_cursor('file:' + self.namepfx + '1', None, None))
        conn.close()

    # Test backup of a file in a 'wt' process.
    def test_backup_file(self):
        self.populate()
        os.mkdir(self.dir)
        for i in range(1, 2):
            self.runWt(
                ['backup', '-t', 'file:' + self.namepfx + '1', self.dir])
        for n in range(1, 2):
            self.compare('file:' + self.namepfx + str(n))

        # The tables shouldn't be there.
        conn = wiredtiger.wiredtiger_open(self.dir)
        session = conn.open_session()
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            session.open_cursor('table:' + self.namepfx + '1', None, None))
        conn.close()

    """
    # Test backup of random object types.
    def test_illegal_objects(self):
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            self.session.open_cursor(
            'backup:', None, 'target=("colgroup:xxx")'))
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            self.session.open_cursor('backup:', None, 'target=("index:xxx")'))
    """

if __name__ == '__main__':
    wttest.run()
