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
# test_encrypt06.py
#   Test that encryption is effective, it leaves no clear text
#

import os
import wttest
from wtscenario import make_scenarios

# Test encryption, when on, does not leak any information
class test_encrypt06(wttest.WiredTigerTestCase):
    # To test the sodium encryptor, we use secretkey= rather than
    # setting a keyid, because for a "real" (vs. test-only) encryptor,
    # keyids require some kind of key server, and (a) setting one up
    # for testing would be a nuisance and (b) currently the sodium
    # encryptor doesn't support any anyway.
    #
    # Note that secretkey= is apparently not allowed with per-table
    # encryption, so we don't test that.
    #
    # It expects secretkey= to provide a hex-encoded 256-bit chacha20 key.
    # This key will serve for testing purposes.
    sodium_testkey = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'

    key11 = ',keyid=11,secretkey=XYZ'
    key13 = ',keyid=13'
    sodiumkey = ',secretkey=' + sodium_testkey

    # Test with various combinations of tables with or without indices
    # and column groups, also with LSM.  When 'match' is False, we
    # testing a potential misuse of the API: a table is opened with
    # with its own encryption options (different from the system),
    # but the indices and column groups do not specify encryption,
    # so they may get the system encryptor.
    storagetype = [
        ('table', dict(
            uriprefix='table:', use_cg=False, use_index=False, match=True)),
        ('table-idx', dict(
            uriprefix='table:', use_cg=False, use_index=True, match=True)),
        ('table-cg', dict(
            uriprefix='table:', use_cg=True, use_index=False, match=True)),
        ('table-cg-idx', dict(
            uriprefix='table:', use_cg=True, use_index=True, match=True)),
        ('table-idx-unmatch', dict(
            uriprefix='table:', use_cg=False, use_index=True, match=False)),
        ('table-cg-unmatch', dict(
            uriprefix='table:', use_cg=True, use_index=False, match=False)),
        ('table-cg-idx-unmatch', dict(
            uriprefix='table:', use_cg=True, use_index=True, match=False)),
        ('lsm', dict(
            uriprefix='lsm:', use_cg=False, use_index=False, match=True)),
    ]
    encrypt = [
        ('none', dict(
            sys_encrypt='none', sys_encrypt_args='',
            table0_encrypt='none', table0_encrypt_args='',
            table1_encrypt='none', table1_encrypt_args='')),
        ('rotn-implied', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11,
            table0_encrypt=None, table0_encrypt_args='',
            table1_encrypt=None, table1_encrypt_args='')),
        ('rotn-all', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11,
            table0_encrypt='rotn', table0_encrypt_args=key13,
            table1_encrypt='rotn', table1_encrypt_args=key13)),
        ('rotn-sys', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11,
            table0_encrypt='none', table0_encrypt_args='',
            table1_encrypt='none', table1_encrypt_args='')),
        ('rotn-table0', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11,
            table0_encrypt='rotn', table0_encrypt_args=key13,
            table1_encrypt='none', table1_encrypt_args='')),
        ('sodium-implied', dict(
            sys_encrypt='sodium', sys_encrypt_args=sodiumkey,
            table0_encrypt=None, table0_encrypt_args='',
            table1_encrypt=None, table1_encrypt_args='')),
    ]
    scenarios = make_scenarios(encrypt, storagetype)
    nrecords = 1000

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('encryptors', self.sys_encrypt)
        extlist.extension('encryptors', self.table0_encrypt)
        extlist.extension('encryptors', self.table1_encrypt)

    def conn_config(self):
        return 'encryption=(name={0}{1}),'.format(
            self.sys_encrypt, self.sys_encrypt_args)

    def encrypt_table_params(self, name, args):
        if name == None:
            return ''
        else:
            return ',encryption=(name=' + name + args + ')'

    def match_string_in_file(self, fname, match):
        with open(fname, 'rb') as f:
            return (f.read().find(match) != -1)

    def match_string_in_rundir(self, match):
        byte_match = match.encode()
        for fname in os.listdir('.'):
            if self.match_string_in_file(fname, byte_match):
                return True
        return False

    def visible_data(self, table_setting):
        if table_setting == None:
            # No table encryption explicitly set, so we use the system setting
            visible = (self.sys_encrypt == 'none')
        else:
            visible = (table_setting == 'none')
        return visible

    def visible_name(self, table_setting, iskey):
        if table_setting == None:
            # No table encryption explicitly set, so we use the system setting
            visible = (self.sys_encrypt == 'none')
        else:
            visible = (table_setting == 'none')

        # If we have everything in a column group, the key name will not
        # be stored in the column group files.  It will be stored in the
        # system metadata, if that is not encrypted.
        if iskey and self.use_cg and self.sys_encrypt != 'none':
            visible = False
        return visible

    # Create a table, add key/values with specific lengths, then verify them.
    def test_encrypt(self):
        name0 = 'test_encrypt06-0'
        name1 = 'test_encrypt06-1'

        enc0 = self.encrypt_table_params(self.table0_encrypt,
                                        self.table0_encrypt_args)
        enc1 = self.encrypt_table_params(self.table1_encrypt,
                                        self.table1_encrypt_args)

        # This is the clear text that we'll be looking for
        txt0 = 'AbCdEfG'
        txt1 = 'aBcDeFg'
        keyname0 = 'MyKey0Name'
        keyname1 = 'MyKey1Name'
        valname0 = 'MyValue0Name'
        valname1 = 'MyValue1Name'

        # Make a bunch of column group and indices,
        # we want to see if any information is leaked anywhere.
        # The key column and one of the value columns is given a name
        # we will look for as clear text.
        sharedparam = 'key_format=S,value_format=SSSS,' + \
                      'columns=({},{},v1,v2,v3),'

        s = self.session
        pfx = self.uriprefix

        cgparam = 'colgroups=(g00,g01)' if self.use_cg else ''
        s.create(pfx + name0, sharedparam.format(keyname0, valname0) + \
                 cgparam + enc0)

        if not self.match:
            enc0 = ''
        if self.use_cg:
            s.create('colgroup:' + name0 + ':g00',
                     'columns=({},v1)'.format(valname0) + enc0)
            s.create('colgroup:' + name0 + ':g01', 'columns=(v2,v3)' + enc0)
        if self.use_index:
            s.create('index:' + name0 + ':i00',
                     'columns=({})'.format(valname0) + enc0)
            s.create('index:' + name0 + ':i01', 'columns=(v1,v2)' + enc0)
            s.create('index:' + name0 + ':i02', 'columns=(v3)' + enc0)

        cgparam = 'colgroups=(g10,g11)' if self.use_cg else ''
        s.create(pfx + name1, sharedparam.format(keyname1, valname1) + \
                 cgparam + enc1)

        if not self.match:
            enc1 = ''
        if self.use_cg:
            s.create('colgroup:' + name1 + ':g10',
                     'columns=({},v1)'.format(valname1) + enc1)
            s.create('colgroup:' + name1 + ':g11', 'columns=(v2,v3)' + enc1)
        if self.use_index:
            s.create('index:' + name1 + ':i10',
                     'columns=({})'.format(valname1) + enc1)
            s.create('index:' + name1 + ':i11', 'columns=(v1,v2)' + enc1)
            s.create('index:' + name1 + ':i12', 'columns=(v3)' + enc1)

        c0 = s.open_cursor(pfx + name0, None)
        c1 = s.open_cursor(pfx + name1, None)
        for idx in range(1,self.nrecords):
            c0.set_key(str(idx) + txt0)
            c1.set_key(str(idx) + txt1)
            c0.set_value(txt0 * (idx % 97), txt0 * 3, txt0 * 5, txt0 * 7)
            c1.set_value(txt1 * (idx % 97), txt1 * 3, txt1 * 5, txt1 * 7)
            c0.insert()
            c1.insert()

        c0.close()
        c1.close()

        # Force everything to disk so we can examine it
        self.close_conn()

        if self.match:
            # Key and value names are encrypted according to the
            # encryption level on the associated table.
            self.assertEqual(self.visible_data(self.table0_encrypt),
                self.match_string_in_rundir(txt0))
            self.assertEqual(self.visible_name(self.table0_encrypt, True),
                self.match_string_in_rundir(keyname0))
            self.assertEqual(self.visible_name(self.table0_encrypt, False),
                self.match_string_in_rundir(valname0))

            self.assertEqual(self.visible_data(self.table1_encrypt),
                self.match_string_in_rundir(txt1))
            self.assertEqual(self.visible_name(self.table1_encrypt, True),
                self.match_string_in_rundir(keyname1))
            self.assertEqual(self.visible_name(self.table1_encrypt, False),
                self.match_string_in_rundir(valname1))
        else:
            # If the encryption config for indices and column groups is blank,
            # we make a conservative check - if we specified encryption on the
            # table, none of our data or key/value names should be exposed.
            #
            # If we have system encryption on, set table encryption to 'none',
            # and set the index or column group config to blank, we technically
            # should get no encryption for names or data. That currently
            # doesn't work (CGs and indices instead will be encrypted),
            # so we don't cover that case.
            if self.table0_encrypt != 'none':
                self.assertFalse(self.match_string_in_rundir(txt0))
                self.assertFalse(self.match_string_in_rundir(keyname0))
                self.assertFalse(self.match_string_in_rundir(valname0))
            if self.table1_encrypt != 'none':
                self.assertFalse(self.match_string_in_rundir(txt1))
                self.assertFalse(self.match_string_in_rundir(keyname1))
                self.assertFalse(self.match_string_in_rundir(valname1))

if __name__ == '__main__':
    wttest.run()
