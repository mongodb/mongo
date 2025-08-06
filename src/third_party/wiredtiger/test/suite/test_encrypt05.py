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
# test_encrypt05.py
#   Test some error conditions with quoted escaped characters.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

# Test error conditions with quoted escaped characters.
class test_encrypt05(wttest.WiredTigerTestCase):
    sys_encrypt = 'rotn'
    sys_encrypt_args = ',keyid=11'
    escaped_characters = [
        ('check newline', dict(escaped_char='\n')),
        ('check carriage return', dict(escaped_char='\r')),
        ('check tab', dict(escaped_char='\t')),
        ('check backspace', dict(escaped_char='\b')),
    ]
    scenarios = make_scenarios(escaped_characters)

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('encryptors', self.sys_encrypt)

    def conn_config(self):
        return 'encryption=(name={0}{1}),'.format(
            self.sys_encrypt, self.sys_encrypt_args)

    # Open connection with a config that has a malformed keyid.
    # The keyid is malformed because it contains a quoted escaped character.
    def test_encrypt(self):
        sys_encrypt_args = f',keyid=\"11{self.escaped_char}\"' # Intentionally malformed keyid
        config = 'encryption=(name={0}{1}),'.format(
            self.sys_encrypt, sys_encrypt_args)

        try:
            self.reopen_conn(config=config)
        except wiredtiger.WiredTigerError as e:
            # If we get an error, it should be about the malformed keyid.
            self.assertTrue('Invalid argument' in str(e), f"Unexpected error: {e}")

        # Reopen the connection with a valid config to avoid teardown errors using the
        # problematic config that had the malformed keyid.
        config = ""
        self.reopen_conn(config=config)

        self.ignoreStderrPatternIfExists("Unexpected escaped character: Invalid argument")
