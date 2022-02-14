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
# test_encrypt08.py
#   Test some error conditions with the libsodium encryption extension.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

#
# Test sodium encryption configuration.
# This exercises the error paths in the encryptor's customize method when
# used for system (not per-table) encryption.
#
class test_encrypt08(wttest.WiredTigerTestCase):
    uri = 'file:test_encrypt08'

    # To test the sodium encryptor, we use secretkey= rather than
    # setting a keyid, because for a "real" (vs. test-only) encryptor,
    # keyids require some kind of key server, and (a) setting one up
    # for testing would be a nuisance and (b) currently the sodium
    # encryptor doesn't support any anyway.
    #
    # It expects secretkey= to provide a hex-encoded 256-bit chacha20 key.
    # This key will serve for testing purposes.
    sodium_testkey = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'

    encrypt_type = [
        ('nokey',    dict( sys_encrypt='',
                           msg='/no key given/')),
        ('keyid',    dict( sys_encrypt='keyid=123',
                           msg='/keyids not supported/')),
        ('twokeys',  dict( sys_encrypt='keyid=123,secretkey=' + sodium_testkey,
                           msg='/keys specified with both/')),
        ('nothex',   dict( sys_encrypt='secretkey=plop',
                           msg='/secret key not hex/')),
        ('badsize',  dict( sys_encrypt='secretkey=0123456789abcdef',
                           msg='/wrong secret key length/')),
    ]
    scenarios = make_scenarios(encrypt_type)

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('encryptors', 'sodium')

    # Do not use conn_config to set the encryption, because that sets
    # the encryption during open when we don't have control and can't
    # catch exceptions. Instead we'll let the frameork open without
    # encryption and then reopen ourselves. This seems to behave as
    # desired (we get the intended errors from inside the encryptor)
    # even though one might expect it to fail because it's reopening
    # the database with different encryption. (If in the future it starts
    # doing that, the workaround is to override setUpConnectionOpen.
    # I'm not doing that now because it's quite a bit messier.)

    # (Re)open the database with bad encryption config.
    def test_encrypt(self):
        sysconfig = 'encryption=(name=sodium,{0}),'.format(self.sys_encrypt)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:
                   self.reopen_conn(config = sysconfig),
             self.msg)

if __name__ == '__main__':
    wttest.run()
