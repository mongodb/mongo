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
# test_encrypt09.py
#   Test some error conditions with the libsodium encryption extension.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

#
# Test sodium encryption configuration.
# This exercises the error paths in the encryptor's customize method when
# used for per-table encryption.
#
class test_encrypt09(wttest.WiredTigerTestCase):
    uri = 'file:test_encrypt09'

    # To test the sodium encryptor, we use secretkey= rather than
    # setting a keyid, because for a "real" (vs. test-only) encryptor,
    # keyids require some kind of key server, and (a) setting one up
    # for testing would be a nuisance and (b) currently the sodium
    # encryptor doesn't support any anyway.
    #
    # It expects secretkey= to provide a hex-encoded 256-bit chacha20 key.
    # This key will serve for testing purposes.
    sodium_testkey = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'

    # The nokey case is not an error; if no key is given no separate encryptor is
    # generated and no error occurs.

    # Note that the twokeys, nothex, and badsize cases do not (currently) get to
    # the extension at all because (apparently) secretkey= is not allowed for
    # per-table encryption.

    encrypt_type = [
        ('nokey',    dict( file_encrypt='',
                           msg=None)),
        ('keyid',    dict( file_encrypt='keyid=123',
                           msg='/keyids not supported/')),
        ('twokeys',  dict( file_encrypt='keyid=123,secretkey=' + sodium_testkey,
                           msg='/unknown configuration key: .secretkey.:/')),
        ('nothex',   dict( file_encrypt='secretkey=plop',
                           msg='/unknown configuration key: .secretkey.:/')),
        ('badsize',  dict( file_encrypt='secretkey=0123456789abcdef',
                           msg='/unknown configuration key: .secretkey.:/')),
    ]
    scenarios = make_scenarios(encrypt_type)

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('encryptors', 'sodium')

    def conn_config(self):
        return 'encryption=(name=sodium,secretkey={0}),'.format(self.sodium_testkey)

    # Create a table with encryption values that are in error.
    def test_encrypt(self):
        params = 'key_format=S,value_format=S,encryption=(name=sodium,' + self.file_encrypt + ')'

        if self.msg is None:
            self.session.create(self.uri, params)
        else:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.create(self.uri, params), self.msg)

if __name__ == '__main__':
    wttest.run()
