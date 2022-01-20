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

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

# test_util19.py
#   Utilities: wt downgrade
class test_util19(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util19.a'
    uri = 'table:' + tablename
    entries = 100
    log_max = "100K"
    log_latest_compat = 5

    create_release = [
        ('def', dict(create_rel='none')),
        ('100', dict(create_rel="10.0")),
        ('33', dict(create_rel="3.3")),
        ('32', dict(create_rel="3.2")),
        ('31', dict(create_rel="3.1")),
        ('30', dict(create_rel="3.0")),
        ('26', dict(create_rel="2.6")),
    ]

    downgrade_release = [
        ('100_rel', dict(downgrade_rel="10.0", log_downgrade_compat=5)),
        ('33_rel', dict(downgrade_rel="3.3", log_downgrade_compat=4)),
        ('32_rel', dict(downgrade_rel="3.2", log_downgrade_compat=3)),
        ('31_rel', dict(downgrade_rel="3.1", log_downgrade_compat=3)),
        ('30_rel', dict(downgrade_rel="3.0", log_downgrade_compat=2)),
        ('26_rel', dict(downgrade_rel="2.6", log_downgrade_compat=1)),
    ]

    scenarios = make_scenarios(create_release, downgrade_release)

    def conn_config(self):
        conf_str = 'log=(enabled,file_max=%s,remove=false),' % self.log_max
        if (self.create_rel != 'none'):
            conf_str += 'compatibility=(release="%s"),' % (self.create_rel)
        return conf_str

    def test_downgrade(self):
        """
        Run wt downgrade on our created database and test its new compatibility version.
        """
        # Create the initial database at the compatibility level established by
        # the connection config ('create_rel').
        self.session.create(self.uri, 'key_format=S,value_format=S')
        c = self.session.open_cursor(self.uri, None)
        # Populate the table to generate some log files.
        for i in range(self.entries):
            key = 'KEY' + str(i)
            val = 'VAL' + str(i)
            c[key] = val
        c.close()

        # Call the downgrade utility to reconfigure our database with the specified compatibility version.
        wt_config = 'log=(enabled,file_max=%s,remove=false),verbose=[log]' % self.log_max
        downgrade_opt = '-V %s' % self.downgrade_rel
        self.runWt(['-C', wt_config , 'downgrade', downgrade_opt], reopensession=False, outfilename='downgrade.out')
        # Based on the downgrade version we can test if the corresponding log compatibility version
        # has been set.
        compat_str = '/WT_CONNECTION\.reconfigure: .*: COMPATIBILITY: Version now %d/' % self.log_downgrade_compat
        if self.log_downgrade_compat != self.log_latest_compat:
            self.check_file_contains('downgrade.out', compat_str)
        else:
            self.check_file_not_contains('downgrade.out', compat_str)

if __name__ == '__main__':
    wttest.run()
