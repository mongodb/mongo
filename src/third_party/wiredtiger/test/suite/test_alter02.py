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

import sys, wiredtiger, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtscenario import make_scenarios

# test_alter02.py
#    Smoke-test the session alter operations.
class test_alter02(TieredConfigMixin, wttest.WiredTigerTestCase):
    entries = 500
    # Binary values.
    value = u'\u0001\u0002abcd\u0003\u0004'
    value2 = u'\u0001\u0002dcba\u0003\u0004'

    conn_log = [
        ('conn-always-logged', dict(conncreate=True, connreopen=True)),
        ('conn-create-logged', dict(conncreate=True, connreopen=False)),
        ('conn-reopen-logged', dict(conncreate=False, connreopen=True)),
        ('conn-never-logged', dict(conncreate=False, connreopen=False)),
    ]

    types = [
        ('file', dict(uri='file:', use_cg=False, use_index=False)),
        ('lsm', dict(uri='lsm:', use_cg=False, use_index=False)),
        ('table-cg', dict(uri='table:', use_cg=True, use_index=False)),
        ('table-index', dict(uri='table:', use_cg=False, use_index=True)),
        ('table-simple', dict(uri='table:', use_cg=False, use_index=False)),
    ]

    tables = [
        ('always-logged', dict(name='table0', logcreate=True, logalter=True)),
        ('create-logged', dict(name='table1', logcreate=True, logalter=False)),
        ('alter-logged', dict(name='table2', logcreate=False, logalter=True)),
        ('never-logged', dict(name='table3', logcreate=False, logalter=False)),
    ]

    reopen = [
        ('no-reopen', dict(reopen=False)),
        ('reopen', dict(reopen=True)),
    ]
    tiered_storage_sources = gen_tiered_storage_sources()
    scenarios = make_scenarios(tiered_storage_sources, conn_log, types, tables, reopen)

    # This test varies the log setting.  Override the standard methods.
    def setUpConnectionOpen(self, dir):
        return None
    def setUpSessionOpen(self, conn):
        return None
    def ConnectionOpen(self):
        self.home = '.'
        
        tiered_config = self.conn_config()
        tiered_config += self.extensionsConfig()
        # In case the open starts additional threads, flush first to avoid confusion.
        sys.stdout.flush()

        conn_params = 'create,log=(file_max=100K,remove=false,%s)' % self.uselog
        if tiered_config != '':
            conn_params += ',' + tiered_config

        try:
            self.conn = wiredtiger.wiredtiger_open(self.home, conn_params)
        except wiredtiger.WiredTigerError as e:
            print("Failed conn at '%s' with config '%s'" % (dir, conn_params))
        self.session = self.conn.open_session()

    # Verify the metadata string for this URI and that its setting in the
    # metadata file is correct.
    def verify_metadata(self, metastr):
        if metastr == '':
            return
        cursor = self.session.open_cursor('metadata:', None, None)
        #
        # Walk through all the metadata looking for the entries that are
        # the file URIs for components of the table.
        #
        found = False
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            key = cursor.get_key()
            check_meta = ((key.find("lsm:") != -1 or key.find("file:") != -1) \
                and key.find(self.name) != -1)
            if check_meta:
                value = cursor[key]
                found = True
                self.assertTrue(value.find(metastr) != -1)
        cursor.close()
        self.assertTrue(found == True)

    # Verify the data in the log.  If the data should be logged we write one
    # value.  If it should not be logged, we write a different value.
    def verify_logrecs(self, expected_keys):
        c = self.session.open_cursor('log:', None, None)
        count = 0
        while c.next() == 0:
            # lsn.file, lsn.offset, opcount
            keys = c.get_key()
            # txnid, rectype, optype, fileid, logrec_key, logrec_value
            values = c.get_value()
            if self.value.encode() in values[5]:     # logrec_value
                count += 1
            self.assertFalse(self.value2.encode() in values[5])
        c.close()
        #
        # We check that we saw the expected keys at twice the rate because
        # the log cursor, for each commit record, will first return the entire,
        # full record, and then return the individual operation. We will detect
        # the string in both records.
        self.assertEqual(count, expected_keys * 2)

    # Alter: Change the log setting after creation
    def test_alter02_log(self):
        if self.is_tiered_scenario() and (self.uri == 'lsm:' or self.uri == 'file:'):
            self.skipTest('Tiered storage does not support LSM or file URIs.')
        
        uri = self.uri + self.name
        create_params = 'key_format=i,value_format=S,'
        complex_params = ''

        # Set up logging for the connection.
        if self.conncreate:
            self.uselog = 'enabled=true'
            conn_logged = 1
        else:
            self.uselog = 'enabled=false'
            conn_logged = 0
        self.ConnectionOpen()

        # Set up logging for the table.
        if self.logcreate:
            log_param = 'log=(enabled=true)'
            table_logged = 1
        else:
            log_param = 'log=(enabled=false)'
            table_logged = 0
        create_params += '%s,' % log_param
        complex_params += '%s,' % log_param

        cgparam = ''
        if self.use_cg or self.use_index:
            cgparam = 'columns=(k,v),'
        if self.use_cg:
            cgparam += 'colgroups=(g0),'

        self.session.create(uri, create_params + cgparam)
        # Add in column group or index settings.
        if self.use_cg:
            cgparam = 'columns=(v),'
            suburi = 'colgroup:' + self.name + ':g0'
            self.session.create(suburi, complex_params + cgparam)
        if self.use_index:
            suburi = 'index:' + self.name + ':i0'
            self.session.create(suburi, complex_params + cgparam)

        # Put some data in table.
        c = self.session.open_cursor(uri, None)
        if self.logcreate:
            myvalue = self.value
        else:
            myvalue = self.value2
        for k in range(self.entries):
            c[k] = myvalue
        c.close()

        # Verify the logging string in the metadata.
        self.verify_metadata(log_param)

        # Verify the logged operations only if logging is enabled.
        expected_keys = conn_logged * table_logged * self.entries
        if conn_logged != 0:
            self.pr("EXPECTED KEYS 1: " + str(expected_keys))
            self.verify_logrecs(expected_keys)

        # Set the alter setting for the table.
        if self.logalter:
            log_str = 'log=(enabled=true)'
            table_logged = 1
        else:
            log_str = 'log=(enabled=false)'
            table_logged = 0
        alter_param = '%s' % log_str
        special = self.use_cg or self.use_index

        # Set the log setting on the new connection.
        if self.reopen:
            if self.connreopen:
                self.uselog = 'enabled=true'
                conn_logged = 1
            else:
                self.uselog = 'enabled=false'
                conn_logged = 0
            self.conn.close()
            self.ConnectionOpen()

        self.session.alter(uri, alter_param)
        if special:
            self.session.alter(suburi, alter_param)
        self.verify_metadata(log_str)
        # Put some more data in table.
        c = self.session.open_cursor(uri, None)
        if self.logalter:
            myvalue = self.value
        else:
            myvalue = self.value2
        for k in range(self.entries):
            c[k + self.entries] = myvalue
        c.close()
        # If we logged the new connection and the table, add in the
        # number of keys we expect.
        expected_keys += conn_logged * table_logged * self.entries
        # if self.logalter and self.connreopen == self.reopen:
        #     expected_keys += self.entries
        # If we logged the connection at any time then check
        # the log records.
        if self.conncreate or (self.connreopen and self.reopen):
            self.pr("EXPECTED KEYS 2: " + str(expected_keys))
            self.verify_logrecs(expected_keys)

if __name__ == '__main__':
    wttest.run()
