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
# test_compat04.py
# Check compatibility API

import wttest
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

class test_compat04(wttest.WiredTigerTestCase, suite_subprocess):
    # Add enough entries and use a small log size to generate more than
    # one log file.
    entries = 2000
    logmax = "100K"
    tablename = 'test_compat04'
    uri = 'table:' + tablename
    # Declare the log versions that do and do not have prevlsn.
    # Log version 1 does not have the prevlsn record.
    # Log version 2 introduced that record.
    # Log versions 3 and higher continue to have that record.
    min_logv = 2

    # The outline of this test is that we create the database at the
    # create release value. Then we reconfigure the release to the
    # reconfig release value. Then we close and reopen the database with
    # a release and compatibility maximum of that release value. This
    # should be successful for all directions.
    #
    create_release = [
        ('def_rel', dict(create_rel='none', log_crrel=5)),
        ('100_rel', dict(create_rel="10.0", log_crrel=5)),
        ('33_rel', dict(create_rel="3.3", log_crrel=4)),
        ('32_rel', dict(create_rel="3.2", log_crrel=3)),
        ('31_rel', dict(create_rel="3.1", log_crrel=3)),
        ('30_rel', dict(create_rel="3.0", log_crrel=2)),
        ('26_rel', dict(create_rel="2.6", log_crrel=1)),
    ]
    reconfig_release = [
        ('100_rel', dict(rel="10.0", log_rel=5)),
        ('33_rel', dict(rel="3.3", log_rel=4)),
        ('32_rel', dict(rel="3.2", log_rel=3)),
        ('31_rel', dict(rel="3.1", log_rel=3)),
        ('30_rel', dict(rel="3.0", log_rel=2)),
        ('300_rel', dict(rel="3.0.0", log_rel=2)),
        ('26_rel', dict(rel="2.6", log_rel=1)),
    ]
    base_config = [
        ('basecfg_true', dict(basecfg='true')),
        ('basecfg_false', dict(basecfg='false')),
    ]
    scenarios = make_scenarios(create_release, reconfig_release, base_config)

    # This test creates scenarios that lead to errors. This is different
    # than compat02 because it is testing errors (or success) using the
    # compatibility settings on the initial database creation.
    def conn_config(self):
        config_str = 'create,config_base=%s,' % self.basecfg
        log_str = 'log=(enabled,file_max=%s,remove=false),' % self.logmax
        compat_str = ''
        if (self.create_rel != 'none'):
            compat_str += 'compatibility=(release="%s"),' % self.create_rel
        config_str += log_str + compat_str
        self.pr("Conn config:" + config_str)
        return config_str

    def test_compat04(self):
        #
        # Create initial database at the compatibility level requested
        # and a table with some data.
        #
        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri, None)
        #
        # Add some entries to generate log files.
        #
        for i in range(self.entries):
            c[i] = i + 1
        c.close()

        # Reconfigure and close the connection. Then reopen with that release.
        # We expect success.
        config_str = 'compatibility=(release=%s)' % self.rel
        self.conn.reconfigure(config_str)
        self.conn.close()

        config_str = 'compatibility=(release=%s,require_max=%s)' % (self.rel, self.rel)
        conn = self.wiredtiger_open('.', config_str)
        conn.close()

if __name__ == '__main__':
    wttest.run()

if __name__ == '__main__':
    wttest.run()
