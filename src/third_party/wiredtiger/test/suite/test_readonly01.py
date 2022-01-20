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
# test_readonly01.py
#   Readonly: Test readonly mode.
#

import fnmatch, os, shutil, time
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
import wttest

class test_readonly01(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_readonly01'
    create = True
    entries = 10000

    #
    # We want a list of directory writable or readonly.
    #
    basecfg_list = [
        ('basecfg', dict(basecfg='config_base=true,operation_tracking=(enabled=false),')),
        ('no_basecfg', dict(basecfg='config_base=false,operation_tracking=(enabled=false),')),
    ]
    dir_list = [
        ('write', dict(dirchmod=False)),
        ('readonly', dict(dirchmod=True)),
    ]
    log_list = [
        ('logging', dict(logcfg='log=(enabled,file_max=100K,remove=false),')),
        ('no_logging', dict(logcfg='log=(enabled=false),')),
    ]

    types = [
        ('lsm', dict(tabletype='lsm', uri='lsm',
                    create_params = 'key_format=i,value_format=i')),
        ('file-row', dict(tabletype='row', uri='file',
                    create_params = 'key_format=i,value_format=i')),
        ('file-var', dict(tabletype='var', uri='file',
                    create_params = 'key_format=r,value_format=i')),
        ('file-fix', dict(tabletype='fix', uri='file',
                    create_params = 'key_format=r,value_format=8t')),
        ('table-row', dict(tabletype='row', uri='table',
                    create_params = 'key_format=i,value_format=i')),
        ('table-var', dict(tabletype='var', uri='table',
                    create_params = 'key_format=r,value_format=i')),
        ('table-fix', dict(tabletype='fix', uri='table',
                    create_params = 'key_format=r,value_format=8t')),
    ]

    scenarios = make_scenarios(basecfg_list, dir_list, log_list, types)

    def conn_config(self):
        params = \
            'error_prefix="%s",' % self.shortid() + \
            '%s' % self.logcfg + \
            '%s' % self.basecfg
        if self.create:
            conn_params = 'create,' + params
        else:
            conn_params = 'readonly=true,' + params
        return conn_params

    def close_reopen(self):
        ''' Close the connection and reopen readonly'''
        #
        # close the original connection.  If needed, chmod the
        # database directory to readonly mode.  Then reopen the
        # connection with readonly.
        #
        self.close_conn()
        #
        # The chmod command is not fully portable to windows.
        #
        if self.dirchmod and os.name == 'posix':
            for f in os.listdir(self.home):
                if os.path.isfile(f):
                    os.chmod(f, 0o444)
            os.chmod(self.home, 0o555)
        self.conn = self.setUpConnectionOpen(self.home)
        self.session = self.setUpSessionOpen(self.conn)

    def readonly(self):
        # Here's the strategy:
        #    - Create a table.
        #    - Insert data into table.
        #    - Close connection.
        #    - Possibly chmod to readonly
        #    - Open connection readonly
        #    - Confirm we can read the data.
        #
        tablearg = self.uri + ':' + self.tablename
        self.session.create(tablearg, self.create_params)
        c = self.session.open_cursor(tablearg, None, None)
        for i in range(self.entries):
            c[i+1] = i % 255
        # Close the connection.  Reopen readonly
        self.create = False
        self.close_reopen()
        c = self.session.open_cursor(tablearg, None, None)
        i = 0
        for key, value in c:
            self.assertEqual(i+1, key)
            self.assertEqual(i % 255, value)
            i += 1
        self.assertEqual(i, self.entries)
        self.pr('Read %d entries' % i)
        c.close()
        self.create = True

    def test_readonly(self):
        if self.dirchmod and os.name == 'posix':
            with self.expectedStderrPattern('Permission'):
                self.readonly()
        else:
            self.readonly()

if __name__ == '__main__':
    wttest.run()
