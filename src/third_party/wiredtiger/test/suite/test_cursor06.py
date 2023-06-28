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
# [TEST_TAGS]
# cursors:reconfigure
# [END_TAGS]

import wiredtiger, wttest
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# test_cursor06.py
#    Test cursor reconfiguration.
class test_cursor06(wttest.WiredTigerTestCase):
    name = 'reconfigure'
    scenarios = make_scenarios([
        ('file-f', dict(type='file:', keyfmt='r', valfmt='8t', dataset=SimpleDataSet)),
        ('file-r', dict(type='file:', keyfmt='r', valfmt='S', dataset=SimpleDataSet)),
        ('file-S', dict(type='file:', keyfmt='S', valfmt='S', dataset=SimpleDataSet)),
        ('lsm-S', dict(type='lsm:', keyfmt='S', valfmt='S', dataset=SimpleDataSet)),
        ('table-f', dict(type='table:', keyfmt='r', valfmt='8t', dataset=SimpleDataSet)),
        ('table-r', dict(type='table:', keyfmt='r', valfmt='S', dataset=SimpleDataSet)),
        ('table-S', dict(type='table:', keyfmt='S', valfmt='S', dataset=SimpleDataSet)),
        ('table-r-complex', dict(type='table:', keyfmt='r', valfmt=None,
            dataset=ComplexDataSet)),
        ('table-S-complex', dict(type='table:', keyfmt='S', valfmt=None,
            dataset=ComplexDataSet)),
        ('table-S-complex-lsm', dict(type='table:', keyfmt='S', valfmt=None,
            dataset=ComplexLSMDataSet)),
    ])

    def populate(self, uri):
        self.ds = self.dataset(self, uri, 100, key_format=self.keyfmt, value_format=self.valfmt)
        self.ds.populate()

    def set_kv(self, cursor):
        cursor.set_key(self.ds.key(10))
        cursor.set_value(self.ds.value(10))

    @wttest.skip_for_hook("timestamp", "crashes on final connection close")  # FIXME-WT-9809
    def test_reconfigure_overwrite(self):
        uri = self.type + self.name
        for open_config in (None, "overwrite=0", "overwrite=1"):
            self.session.drop(uri, "force")
            self.populate(uri)
            cursor = self.ds.open_cursor(uri, None, open_config)
            if open_config != "overwrite=0":
                self.set_kv(cursor)
                cursor.insert()
            for i in range(0, 10):
                cursor.reconfigure("overwrite=0")
                self.set_kv(cursor)
                self.assertRaises(wiredtiger.WiredTigerError,
                                  lambda: cursor.insert())
                cursor.reconfigure("overwrite=1")
                self.set_kv(cursor)
                cursor.insert()
            cursor.close()

    def test_reconfigure_readonly(self):
        uri = self.type + self.name
        for open_config in (None, "readonly=0", "readonly=1"):
            self.session.drop(uri, "force")
            self.populate(uri)
            cursor = self.ds.open_cursor(uri, None, open_config)
            msg = '/Unsupported cursor/'
            if open_config == "readonly=1":
                self.set_kv(cursor)
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                                  lambda: cursor.update(), msg)
            else:
                self.set_kv(cursor)
                cursor.update()
            cursor.close()

    def test_reconfigure_invalid(self):
        uri = self.type + self.name
        self.populate(uri)
        c = self.ds.open_cursor(uri, None, None)
        c.reconfigure("overwrite=1")
        msg = '/Invalid argument/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: c.reconfigure("xxx=true"), msg)

if __name__ == '__main__':
    wttest.run()
