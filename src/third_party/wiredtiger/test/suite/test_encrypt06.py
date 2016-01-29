#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import os, run, random
import wiredtiger, wttest
from wtscenario import multiply_scenarios, number_scenarios

# Test encryption, when on, does not leak any information
class test_encrypt06(wttest.WiredTigerTestCase):

    key11 = ',keyid=11,secretkey=XYZ'
    key13 = ',keyid=13'

    # Test with various combinations of tables with or without indices
    # and column groups, also with LSM.  When 'match' is False, we
    # testing a potential misuse of the API: a table is opened with
    # with its own encryption options (different from the system),
    # but the indices and column groups do not specify encryption,
    # so they'll get the system encryptor.
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
            sys_encrypt='none', sys_encrypt_args='', encryptmeta=False,
            file0_encrypt='none', file0_encrypt_args='', encrypt0=False,
            file1_encrypt='none', file1_encrypt_args='', encrypt1=False)),
        ('rotn-implied', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11, encryptmeta=True,
            file0_encrypt=None, file0_encrypt_args='', encrypt0=True,
            file1_encrypt=None, file1_encrypt_args='', encrypt1=True)),
        ('rotn-all', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11, encryptmeta=True,
            file0_encrypt='rotn', file0_encrypt_args=key13, encrypt0=True,
            file1_encrypt='rotn', file1_encrypt_args=key13, encrypt1=True)),
        ('rotn-sys', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11, encryptmeta=True,
            file0_encrypt='none', file0_encrypt_args='', encrypt0=False,
            file1_encrypt='none', file1_encrypt_args='', encrypt1=False)),
        ('rotn-file0', dict(
            sys_encrypt='rotn', sys_encrypt_args=key11, encryptmeta=True,
            file0_encrypt='rotn', file0_encrypt_args=key13, encrypt0=True,
            file1_encrypt='none', file1_encrypt_args='', encrypt1=False)),
    ]
    scenarios = number_scenarios(multiply_scenarios('.', encrypt, storagetype))
    nrecords = 1000

    # Override WiredTigerTestCase, we have extensions.
    def setUpConnectionOpen(self, dir):
        encarg = 'encryption=(name={0}{1}),'.format(
            self.sys_encrypt, self.sys_encrypt_args)
        comparg = ''
        extarg = self.extensionArg([('encryptors', self.sys_encrypt),
            ('encryptors', self.file0_encrypt),
            ('encryptors', self.file1_encrypt)])
        self.open_params = 'create,error_prefix="{0}: ",{1}{2}{3}'.format(
                self.shortid(), encarg, comparg, extarg)
        conn = self.wiredtiger_open(dir, self.open_params)
        self.pr(`conn`)
        return conn

    # Return the wiredtiger_open extension argument for a shared library.
    def extensionArg(self, exts):
        extfiles = []
        for ext in exts:
            (dirname, name) = ext
            if name != None and name != 'none':
                testdir = os.path.dirname(__file__)
                extdir = os.path.join(run.wt_builddir, 'ext', dirname)
                extfile = os.path.join(
                    extdir, name, '.libs', 'libwiredtiger_' + name + '.so')
                if not os.path.exists(extfile):
                    self.skipTest('extension "' + extfile + '" not built')
                if not extfile in extfiles:
                    extfiles.append(extfile)
        if len(extfiles) == 0:
            return ''
        else:
            return ',extensions=["' + '","'.join(extfiles) + '"]'

    def encrypt_file_params(self, name, args):
        if name == None:
            return ''
        else:
            return ',encryption=(name=' + name + args + ')'


    def match_string_in_file(self, fname, match):
        with open(fname, 'rb') as f:
            return (f.read().find(match) != -1)

    def match_string_in_rundir(self, match):
        for fname in os.listdir('.'):
            if self.match_string_in_file(fname, match):
                return True
        return False

    def expected_encryption(self, exp):
        expect = exp
        # If we're expecting it to be unencrypted, but we (errantly)
        # did not specify encryption on indices/columngroups,
        # then column groups (if they exist) will be encrypted -
        # there will be no data in the main table to be unencrypted.
        if self.sys_encrypt != 'none' and not self.match and self.use_cg:
            expect = True
        return expect

    # Create a table, add key/values with specific lengths, then verify them.
    def test_encrypt(self):
        name0 = 'test_encrypt06-0'
        name1 = 'test_encrypt06-1'

        enc0 = self.encrypt_file_params(self.file0_encrypt,
                                        self.file0_encrypt_args)
        enc1 = self.encrypt_file_params(self.file1_encrypt,
                                        self.file1_encrypt_args)

        # This is the clear text that we'll be looking for
        txt0 = 'AbCdEfG'
        txt1 = 'aBcDeFg'

        # Make a bunch of column group and indices,
        # we want to see if any information is leaked anywhere.
        sharedparam = 'key_format=S,value_format=SSSS,' + \
                      'columns=(MyKeyName,v0,v1,v2,v3),'

        s = self.session
        pfx = self.uriprefix

        cgparam = 'colgroups=(g00,g01)' if self.use_cg else ''
        s.create(pfx + name0, sharedparam + cgparam + enc0)

        # Having unmatched encryption for colgroup or index is
        # not recommended, but we check it.
        if not self.match:
            enc0 = ''
        if self.use_cg:
            s.create('colgroup:' + name0 + ':g00', 'columns=(v0,v1)' + enc0)
            s.create('colgroup:' + name0 + ':g01', 'columns=(v2,v3)' + enc0)
        if self.use_index:
            s.create('index:' + name0 + ':i00', 'columns=(v0)' + enc0)
            s.create('index:' + name0 + ':i01', 'columns=(v1,v2)' + enc0)
            s.create('index:' + name0 + ':i02', 'columns=(v3)' + enc0)

        cgparam = 'colgroups=(g10,g11)' if self.use_cg else ''
        s.create(pfx + name1, sharedparam + cgparam + enc1)

        if not self.match:
            enc1 = ''
        if self.use_cg:
            s.create('colgroup:' + name1 + ':g10', 'columns=(v0,v1)' + enc1)
            s.create('colgroup:' + name1 + ':g11', 'columns=(v2,v3)' + enc1)
        if self.use_index:
            s.create('index:' + name1 + ':i10', 'columns=(v0)' + enc1)
            s.create('index:' + name1 + ':i11', 'columns=(v1,v2)' + enc1)
            s.create('index:' + name1 + ':i12', 'columns=(v3)' + enc1)

        c0 = s.open_cursor(pfx + name0, None)
        c1 = s.open_cursor(pfx + name1, None)
        for idx in xrange(1,self.nrecords):
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

        self.assertEqual(self.encryptmeta,
                         not self.match_string_in_rundir('MyKeyName'))

        self.assertEqual(self.expected_encryption(self.encrypt0),
                         not self.match_string_in_rundir(txt0))
        self.assertEqual(self.expected_encryption(self.encrypt1),
                         not self.match_string_in_rundir(txt1))


if __name__ == '__main__':
    wttest.run()
