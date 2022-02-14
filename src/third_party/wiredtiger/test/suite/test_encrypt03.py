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
# test_encrypt03.py
#   Test some error conditions with encryption.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

# Test basic encryption
class test_encrypt03(wttest.WiredTigerTestCase):

    types = [
        ('table', dict(uri='table:test_encrypt03')),
    ]
    encrypt = [
        ('none', dict( sys_encrypt='none', sys_encrypt_args='',
            file_encrypt='rotn', file_encrypt_args=',keyid=13')),
        # This case is now permitted: it the system encryption is inherited by
        # the table.
        #('noname', dict( sys_encrypt='rotn', sys_encrypt_args=',keyid=11',
        #    file_encrypt='none', file_encrypt_args=',keyid=13')),
    ]
    scenarios = make_scenarios(types, encrypt)

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('encryptors', self.sys_encrypt)
        extlist.extension('encryptors', self.file_encrypt)

    def conn_config(self):
        return 'encryption=(name={0}{1}),'.format(
            self.sys_encrypt, self.sys_encrypt_args)

    # Create a table with encryption values that are in error.
    def test_encrypt(self):
        params = 'key_format=S,value_format=S,encryption=(name='
        if self.file_encrypt != None:
            params += self.file_encrypt
        if self.file_encrypt_args != None:
            params += ',keyid=' + self.file_encrypt_args
        params += ')'

        # All error messages so far have this in common.
        msg = '/to be set: Invalid argument/'

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.create(self.uri, params), msg)

if __name__ == '__main__':
    wttest.run()
