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
# test_compat02.py
# Check compatibility API

import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

class test_compat02(wttest.WiredTigerTestCase, suite_subprocess):
    # Add enough entries and use a small log size to generate more than
    # one log file.
    entries = 2000
    logmax = "100K"
    tablename = 'test_compat02'
    uri = 'table:' + tablename
    # Declare the log versions that do and do not have prevlsn.
    # Log version 1 does not have the prevlsn record.
    # Log version 2 introduced that record.
    # Log versions 3 and higher continue to have that record.
    min_logv = 2

    # Test detecting a not-yet-existing log version. This should
    # hold us for many years.
    future_logv = 20
    future_rel = "20.0"

    # The API uses only the major and minor numbers but accepts with
    # and without the patch number. Test one on release and the
    # required minimum just for testing of parsing.

    compat_create = [
        ('def', dict(create_rel='none', log_create=5)),
        ('112', dict(create_rel="11.2", log_create=5)),
        ('111', dict(create_rel="11.1", log_create=5)),
        ('110', dict(create_rel="11.0", log_create=5)),
        ('100', dict(create_rel="10.0", log_create=5)),
        ('33', dict(create_rel="3.3", log_create=4)),
        ('32', dict(create_rel="3.2", log_create=3)),
        ('31', dict(create_rel="3.1", log_create=3)),
        ('30', dict(create_rel="3.0", log_create=2)),
        ('26', dict(create_rel="2.6", log_create=1)),
    ]

    compat_release = [
        ('def_rel', dict(rel='none', log_rel=5)),
        ('112_rel', dict(rel="11.2", log_rel=5)),
        ('111_rel', dict(rel="11.1", log_rel=5)),
        ('110_rel', dict(rel="11.0", log_rel=5)),
        ('100_rel', dict(rel="10.0", log_rel=5)),
        ('33_rel', dict(rel="3.3", log_rel=4)),
        ('32_rel', dict(rel="3.2", log_rel=3)),
        ('31_rel', dict(rel="3.1", log_rel=3)),
        ('30_rel', dict(rel="3.0", log_rel=2)),
        ('26_rel', dict(rel="2.6", log_rel=1)),
        ('26_patch_rel', dict(rel="2.6.1", log_rel=1)),
    ]

    # Only the maximum version should exist below for each log version
    # i.e. even though 3.1 is also log_max=3 3.2 is above it and also
    # log_max=3 as such we don't need 3.1 in this list.
    # However the exemption to this rule is versions which include a patch
    # number as the patch number will get removed in the conn_reconfig.c
    # This rule exemption applies to the minimum verison check as well.
    compat_max = [
        ('future_max', dict(max_req=future_rel, log_max=future_logv)),
        ('def_max', dict(max_req='none', log_max=5)),
        ('112_max', dict(max_req="11.2", log_max=5)),
        ('33_max', dict(max_req="3.3", log_max=4)),
        ('32_max', dict(max_req="3.2", log_max=3)),
        ('30_max', dict(max_req="3.0", log_max=2)),
        ('26_max', dict(max_req="2.6", log_max=1)),
        ('26_patch_max', dict(max_req="2.6.1", log_max=1)),
    ]

    # Only the minimum version should exist below for each log version.
    compat_min = [
        ('future_min', dict(min_req=future_rel, log_min=future_logv)),
        ('def_min', dict(min_req='none', log_min=5)),
        ('100_min', dict(min_req="10.0", log_min=5)),
        ('33_min', dict(min_req="3.3", log_min=4)),
        ('31_min', dict(min_req="3.1", log_min=3)),
        ('30_min', dict(min_req="3.0", log_min=2)),
        ('26_min', dict(min_req="2.6", log_min=1)),
        ('26_patch_min', dict(min_req="2.6.1", log_min=1)),
    ]
    base_config = [
        ('basecfg_true', dict(basecfg='true')),
        ('basecfg_false', dict(basecfg='false')),
    ]

    scenarios = make_scenarios(compat_create, compat_release, compat_min, compat_max, base_config,
                               prune=100, prunelong=100000)

    def conn_config(self):
        # Set remove=false on the home directory.
        config_str = 'config_base=%s,' % self.basecfg
        log_str = 'log=(enabled,file_max=%s,remove=false),' % self.logmax
        compat_str = ''
        if (self.create_rel != 'none'):
            compat_str += 'compatibility=(release="%s"),' % self.create_rel
        config_str += log_str + compat_str
        self.pr("Conn config:" + config_str)
        return config_str

    def test_compat02(self):
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

        # Close and reopen the connection with the required compatibility
        # version. That configuration needs an existing database for it to be
        # useful. Test for success or failure based on the relative versions
        # configured.

        # Turn on checkpoint verbose to debug a rare occurence of a test
        # hanging, most likely during the checkpoint of conn.close.
        self.pr("Closing connection")
        self.conn.reconfigure('verbose=(checkpoint)')
        with self.expectedStdoutPattern('.'):
            self.conn.close()
        compat_str = ''
        if (self.max_req != 'none'):
            compat_str += 'compatibility=(require_max="%s"),' % self.max_req
        if (self.min_req != 'none'):
            compat_str += 'compatibility=(require_min="%s"),' % self.min_req
        if (self.rel != 'none'):
            compat_str += 'compatibility=(release="%s"),' % self.rel
        log_str = 'log=(enabled,file_max=%s,remove=false),' % self.logmax
        restart_config = log_str + compat_str
        self.pr("Restart conn " + restart_config)

        # We have a lot of error cases. There are too many and they are
        # dependent on the order of the library code so don't check specific
        # error messages. So just determine if an error should occur and
        # make sure it does.
        if ((self.log_min >= self.future_logv) or
          (self.log_max >= self.future_logv) or
          (self.max_req != 'none' and self.log_max < self.log_rel) or
          (self.min_req != 'none' and self.log_min > self.log_rel) or
          (self.max_req != 'none' and self.min_req != 'none' and self.log_max < self.log_min) or
          (self.max_req != 'none' and self.log_max < self.log_create) or
          (self.min_req != 'none' and self.log_min > self.log_create)):
            expect_err = True
        else:
            expect_err = False

        if (expect_err == True):
            msg = '/Version incompatibility detected:/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.wiredtiger_open('.', restart_config), msg)
        else:
            self.pr("EXPECT SUCCESS")
            conn = self.wiredtiger_open('.', restart_config)
            conn.close()

if __name__ == '__main__':
    wttest.run()
