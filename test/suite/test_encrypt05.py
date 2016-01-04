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
# test_encrypt05.py
#   Test raw compression with encryption
#

import os, run, random
import wiredtiger, wttest
from wtscenario import multiply_scenarios, number_scenarios

# Test raw compression with encryption
class test_encrypt05(wttest.WiredTigerTestCase):

    encrypt = [
        ('rotn', dict( sys_encrypt='rotn', sys_encrypt_args=',keyid=11',
            file_encrypt='rotn', file_encrypt_args=',keyid=13')),
    ]
    compress = [
        ('zlib', dict(log_compress='zlib', block_compress='zlib')),
    ]
    scenarios = number_scenarios(multiply_scenarios('.',
                                                    encrypt, compress))

    nrecords = 500
    bigvalue = 'a' * 500 # we use values that will definitely give compression

    # Override WiredTigerTestCase, we have extensions.
    def setUpConnectionOpen(self, dir):
        encarg = 'encryption=(name={0}{1}),'.format(
            self.sys_encrypt, self.sys_encrypt_args)
        comparg = ''
        if self.log_compress != None:
            comparg='log=(compressor={0}),'.format(self.log_compress)
        extarg = self.extensionArg([('encryptors', self.sys_encrypt),
            ('encryptors', self.file_encrypt),
            ('compressors', self.block_compress),
            ('compressors', self.log_compress)])
        conn = self.wiredtiger_open(dir,
            'create,error_prefix="{0}: ",{1}{2}{3}'.format(
                self.shortid(), encarg, comparg, extarg))
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

    def getvalue(self, r, n):
        if n < len(self.bigvalue):
            return self.bigvalue[0: n]
        else:
            diff = n - len(self.bigvalue)
            rchr = ''.join(chr(r.randint(1, 255)) for i in range(diff))
            return self.bigvalue + rchr

    # Create a table, add key/values with specific lengths, then verify them.
    def test_encrypt(self):
        params = 'key_format=S,value_format=S'
        if self.file_encrypt != None:
            params += ',encryption=(name=' + self.file_encrypt + \
                      self.file_encrypt_args + ')'
        if self.block_compress != None:
            params += ',block_compressor=' + self.block_compress
        # Explicitly set max size for leaf page
        params += ',leaf_page_max=8KB'

        # n is the length of the value.  This range is experimentally chosen
        # to be near an edge case for an 8K leaf size for raw compression.
        # We can fit about 10-11 records of this size on the page.  We let
        # the size creep up to the edge case.  The compressor is trying to
        # maximize the number of records that can fit on the fixed size
        # page, and the calculation is modulated by the encryptor's need for
        # a constant buffer growth.
        for n in xrange(1045, 1060, 1):
            uri='table:test_encrypt05-' + str(n)
            self.session.create(uri, params)
            r = random.Random()
            r.seed(0)
            cursor = self.session.open_cursor(uri, None)
            for idx in xrange(1,self.nrecords):
                key = str(idx)
                cursor.set_key(key)
                cursor.set_value(self.getvalue(r, n))
                cursor.insert()
            cursor.close()

            # Force the cache to disk, so we read
            # compressed/encrypted pages from disk.
            self.reopen_conn()

            cursor = self.session.open_cursor(uri, None)
            r.seed(0)
            for idx in xrange(1,self.nrecords):
                key = str(idx)
                cursor.set_key(key)
                self.assertEqual(cursor.search(), 0)
                self.assertEquals(cursor.get_value(), self.getvalue(r, n))
            cursor.close()

if __name__ == '__main__':
    wttest.run()
