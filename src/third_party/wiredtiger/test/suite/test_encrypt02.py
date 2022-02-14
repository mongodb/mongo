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
# [TEST_TAGS]
# encryption
# [END_TAGS]
#
# test_encrypt02.py
#   Encryption using passwords
#

import random
import wttest
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

# Test basic encryption
class test_encrypt02(wttest.WiredTigerTestCase, suite_subprocess):
    # To test the sodium encryptor, we use secretkey= rather than
    # setting a keyid, because for a "real" (vs. test-only) encryptor,
    # keyids require some kind of key server, and (a) setting one up
    # for testing would be a nuisance and (b) currently the sodium
    # encryptor doesn't support any anyway.
    #
    # It expects secretkey= to provide a hex-encoded 256-bit chacha20 key.
    # This key will serve for testing purposes.
    sodium_testkey = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'

    uri = 'file:test_encrypt02'
    encrypt_type = [
        ('noarg', dict( encrypt_args='name=rotn', secret_arg=None)),
        ('keyid', dict( encrypt_args='name=rotn,keyid=11', secret_arg=None)),
        ('pass', dict( encrypt_args='name=rotn', secret_arg='ABC')),
        ('keyid-pass', dict(
            encrypt_args='name=rotn,keyid=11', secret_arg='ABC')),
        ('sodium-pass', dict( encrypt_args='name=sodium', secret_arg=sodium_testkey)),
        # The other combinations for sodium, which are rejected, are checked in encrypt08.
    ]
    scenarios = make_scenarios(encrypt_type)

    def conn_extensions(self, extlist):
        # Load the compression extension, skip the test if missing
        extlist.skip_if_missing = True
        extlist.extension('encryptors', 'rotn')
        extlist.extension('encryptors', 'sodium')

    nrecords = 5000
    bigvalue = "abcdefghij" * 1001    # len(bigvalue) = 10010

    def conn_config(self):
        secretarg = ''
        if self.secret_arg != None:
            secretarg = ',secretkey=' + self.secret_arg
        return 'encryption=({0}{1})'.format(self.encrypt_args, secretarg)

    # Create a table, add keys with both big and small values, then verify them.
    def test_pass(self):
        params = 'key_format=S,value_format=S'
        params += ',encryption=(' + self.encrypt_args + ')'

        self.session.create(self.uri, params)
        cursor = self.session.open_cursor(self.uri, None)
        r = random.Random()
        r.seed(0)
        for idx in range(1,self.nrecords):
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
        for idx in range(1,self.nrecords):
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
