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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_stat07.py
#    Session statistics cursor configurations.
class test_stat_cursor_config(wttest.WiredTigerTestCase):
    pfx = 'test_op_stat_cursor_config'
    uri = [
        ('file',  dict(uri='file:' + pfx, dataset=SimpleDataSet))
    ]
    data_config = [
        ('none', dict(data_config='none', ok=[])),
        ('all',  dict(data_config='all', ok=['empty', 'fast', 'all'])),
        ('fast', dict(data_config='fast', ok=['empty', 'fast']))
    ]
    cursor_config = [
        ('empty', dict(cursor_config='empty')),
        ( 'all', dict(cursor_config='all')),
        ('fast', dict(cursor_config='fast')),
    ]

    scenarios = make_scenarios(uri, data_config, cursor_config)

    # Turn on statistics for this test.
    def conn_config(self):
        return 'statistics=(%s)' % self.data_config

    # For each database/cursor configuration, confirm the right combinations
    # succeed or fail. Traverse the statistics cursor and fetch the statistics.
    def test_stat_cursor_config(self):
        self.dataset(self, self.uri, 100).populate()
        config = 'statistics=('
        if self.cursor_config != 'empty':
            config = config + self.cursor_config
        config = config + ')'
        if self.ok and self.cursor_config in self.ok:
            found = False
            stat_cur = self.session.open_cursor('statistics:session', None, config)

            # A reset on session should reset the statistics values to zero.
            self.session.reset()
            stat_cur.reset()
            while stat_cur.next() == 0:
                [desc, pvalue, value] = stat_cur.get_values()
                self.assertEquals(value, 0)
                found = True
            self.assertEquals(found, True)

        else:
            msg = '/database statistics configuration/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.open_cursor('statistics:session', None, config), msg)

if __name__ == '__main__':
    wttest.run()
