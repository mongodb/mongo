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
import helper, wiredtiger, wttest
from wtscenario import make_scenarios

# test_prefetch01.py
#    Test basic functionality of the prefetch configuration.

class test_prefetch01(wttest.WiredTigerTestCase):
    new_dir = 'new.dir'

    conn_avail = [
        ('available', dict(available=True)),
        ('not-available', dict(available=False))
    ]

    conn_default = [
        ('default-off', dict(default=True)),
        ('default-on', dict(default=False)),
    ]

    session_cfg = [
        ('no-config', dict(scenario='no-config', enabled=False, has_config=False)),
        ('enabled', dict(scenario='enabled', enabled=True, has_config=True)),
        ('not-enabled', dict(scenario='not-enabled', enabled=False, has_config=True)),
    ]

    scenarios = make_scenarios(conn_avail, conn_default, session_cfg)

    def test_prefetch_config(self):
        conn_cfg = 'prefetch=(available=%s,default=%s)' % (str(self.available).lower(), str(self.default).lower())
        session_cfg = ''
        msg = '/pre-fetching cannot be enabled/'

        if self.has_config:
            session_cfg = 'prefetch=(enabled=%s)' % (str(self.enabled).lower())

        os.mkdir(self.new_dir)
        helper.copy_wiredtiger_home(self, '.', self.new_dir)

        if not self.available and self.default:
            # Test that we can't enable a connection's sessions to have pre-fetching when
            # pre-fetching is configured as unavailable.
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.wiredtiger_open(self.new_dir, conn_cfg), msg)
        elif not self.available and self.enabled:
            # Test that we can't enable a specific session to have pre-fetching turned on
            # if pre-fetching is configured as unavailable.
            new_conn = self.wiredtiger_open(self.new_dir, conn_cfg)
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: new_conn.open_session(session_cfg), msg)
        else:
            new_conn = self.wiredtiger_open(self.new_dir, conn_cfg)
            new_session = new_conn.open_session(session_cfg)
            self.assertEqual(new_session.close(), 0)

if __name__ == '__main__':
    wttest.run()
