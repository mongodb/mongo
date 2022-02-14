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

import wttest
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

# test_backup20.py
# Test cursor backup force stop without a checkpoint.
# This reproduces the issue from WT-7027 where we hit an assertion
# because the session was created with snapshot isolation.
class test_backup20(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    uri="table:test"
    nops=1000
    mult=0

    pfx = 'test_backup'

    scenarios = make_scenarios([
        ('default', dict(sess_cfg='')),
        ('read-committed', dict(sess_cfg='isolation=read-committed')),
        ('read-uncommitted', dict(sess_cfg='isolation=read-uncommitted')),
        ('snapshot', dict(sess_cfg='isolation=snapshot')),
    ])

    def session_config(self):
        return self.sess_cfg

    def test_backup20(self):
        self.session.create(self.uri, "key_format=S,value_format=S")

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        config = 'incremental=(enabled,granularity=1M,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()

        # Do a force stop to release resources and reset the system.
        config = 'incremental=(force_stop=true)'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()

        self.session.close()
        self.conn.close()

if __name__ == '__main__':
    wttest.run()
