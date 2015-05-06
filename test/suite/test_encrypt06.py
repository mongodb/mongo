#!/usr/bin/env python
#
# Public Domain 2014-2015 MongoDB, Inc.
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

# TODO - tmp code
def tty_pr(s):
    o = open('/dev/tty', 'w')
    o.write(s + '\n')
    o.close()

# Test raw compression with encryption
class test_encrypt06(wttest.WiredTigerTestCase):

    key11 = ',keyid=11,secretkey=XYZ'
    key13 = ',keyid=13'
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
    scenarios = number_scenarios(encrypt)
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
        conn = wiredtiger.wiredtiger_open(dir, self.open_params)
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
        with open(fname, 'r') as f:
            return (f.read().find(match) != -1)

    def match_string_in_rundir(self, match):
        for fname in os.listdir('.'):
            if self.match_string_in_file(fname, match):
                return True
        return False

    # Create a table, add key/values with specific lengths, then verify them.
    def test_encrypt(self):
        uri0 = 'table:test_encrypt06-0'
        uri1 = 'table:test_encrypt06-1'

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

#        tty_pr('wiredtiger_open params: ' + self.open_params)
#        tty_pr(uri0 + ' create params: ' + sharedparam + enc0)
#        tty_pr(uri1 + ' create params: ' + sharedparam + enc1)

        self.session.create(uri0, sharedparam + 'colgroups=(g00,g01)' + enc0)
        self.session.create(uri1, sharedparam + 'colgroups=(g10,g11)' + enc1)
        self.session.create('colgroup:test_encrypt06-0:g00', 'columns=(v0,v1)')
        self.session.create('colgroup:test_encrypt06-0:g01', 'columns=(v2,v3)')
        self.session.create('colgroup:test_encrypt06-1:g10', 'columns=(v0,v1)')
        self.session.create('colgroup:test_encrypt06-1:g11', 'columns=(v2,v3)')
        self.session.create('index:test_encrypt06-0:i00', 'columns=(v0)')
        self.session.create('index:test_encrypt06-0:i01', 'columns=(v1,v2)')
        self.session.create('index:test_encrypt06-0:i02', 'columns=(v3)')
        self.session.create('index:test_encrypt06-1:i10', 'columns=(v0)')
        self.session.create('index:test_encrypt06-1:i11', 'columns=(v1,v2)')
        self.session.create('index:test_encrypt06-1:i12', 'columns=(v3)')

        c0 = self.session.open_cursor(uri0, None)
        c1 = self.session.open_cursor(uri1, None)
        for idx in xrange(1,self.nrecords):
            c0.set_key(str(idx) + txt0)
            c1.set_key(str(idx) + txt1)
            c0.set_value(txt0 * (idx % 97), txt0 * 3, txt0 * 5, txt0 * 7)
            c1.set_value(txt1 * (idx % 97), txt1 * 3, txt1 * 5, txt1 * 7)
            c1.insert()
            c0.insert()

        c0.close()
        c1.close()
            
        # Force everything to disk so we can examine it
        self.close_conn()

        self.assertEqual(self.encryptmeta,
                         not self.match_string_in_rundir('MyKeyName'))
        self.assertEqual(self.encrypt0,
                         not self.match_string_in_rundir(txt0))
        self.assertEqual(self.encrypt1,
                         not self.match_string_in_rundir(txt1))
        

if __name__ == '__main__':
    wttest.run()
