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
# test_encrypt02.py
#   Encryption using passwords
#

import os, run, random
import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios

# Test basic encryption
class test_encrypt02(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'file:test_encrypt02'
    encrypt_type = [
        ('noarg', dict( encrypt='rotn', encrypt_args='name=rotn',
                        secret_arg=None)),
        ('keyid', dict( encrypt='rotn', encrypt_args='name=rotn,keyid=11',
                        secret_arg=None)),
        ('pass', dict( encrypt='rotn', encrypt_args='name=rotn',
                        secret_arg='ABC')),
        ('keyid-pass', dict( encrypt='rotn', encrypt_args='name=rotn,keyid=11',
                        secret_arg='ABC')),
    ]
    scenarios = number_scenarios(encrypt_type)

    nrecords = 5000
    bigvalue = "abcdefghij" * 1001    # len(bigvalue) = 10010

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

    # Override WiredTigerTestCase, we have extensions.
    def setUpConnectionOpen(self, dir):
        secretarg = ''
        if self.secret_arg != None:
            secretarg = ',secretkey=' + self.secret_arg
        encarg = 'encryption=({0}{1})'.format(self.encrypt_args, secretarg)
        extarg = self.extensionArg([('encryptors', self.encrypt)])
        connarg = 'create,error_prefix="{0}: ",{1},{2}'.format(
            self.shortid(), encarg, extarg)
        conn = self.wiredtiger_open(dir, connarg)
        self.pr(`conn`)
        return conn

    # Create a table, add keys with both big and small values, then verify them.
    def test_pass(self):
        params = 'key_format=S,value_format=S'
        params += ',encryption=(' + self.encrypt_args + ')'

        self.session.create(self.uri, params)
        cursor = self.session.open_cursor(self.uri, None)
        r = random.Random()
        r.seed(0)
        for idx in xrange(1,self.nrecords):
            start = r.randint(0,9)
            key = self.bigvalue[start:r.randint(0,100)] + str(idx)
            val = self.bigvalue[start:r.randint(0,10000)] + str(idx)
            cursor.set_key(key)
            cursor.set_value(val)
            cursor.insert()
        cursor.close()

        # Force the cache to disk, so we read
        # encrypted pages from disk.
        self.reopen_conn()

        cursor = self.session.open_cursor(self.uri, None)
        r.seed(0)
        for idx in xrange(1,self.nrecords):
            start = r.randint(0,9)
            key = self.bigvalue[start:r.randint(0,100)] + str(idx)
            val = self.bigvalue[start:r.randint(0,10000)] + str(idx)
            cursor.set_key(key)
            self.assertEqual(cursor.search(), 0)
            self.assertEquals(cursor.get_value(), val)
        cursor.close()

        wtargs = []
        if self.secret_arg != None:
            wtargs += ['-E', self.secret_arg]
        wtargs += ['dump', self.uri]
        self.runWt(wtargs, outfilename='dump.out')
        self.check_non_empty_file('dump.out')

if __name__ == '__main__':
    wttest.run()
