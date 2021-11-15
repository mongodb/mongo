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
# test_encrypt07.py
#   Salvage encrypted databases
#

import os, run, string, codecs
import wiredtiger, wttest

# If removing this, update test_salvage to not reference here.
import test_salvage

# Run the regular salvage test, but with encryption on
class test_encrypt07(test_salvage.test_salvage):

    uri='table:test_encrypt07'
    sys_encrypt='rotn'
    sys_encrypt_args=',keyid=13'      # This is rot13

    nrecords = 5000
    bigvalue = "abcdefghij" * 1007    # len(bigvalue) = 10070

    def conn_extensions(self, extlist):
        # Load the compression extension, skip the test if missing
        extlist.skip_if_missing = True
        extlist.extension('encryptors', self.sys_encrypt)

    def conn_config(self):
        return 'encryption=(name={0}{1}),'.format(
            self.sys_encrypt, self.sys_encrypt_args)

    def rot13(self, s):
        return codecs.encode(s, 'rot_13')

    # Supplement test_salvage.moreinit.
    # When we're looking in the file for our 'unique' set of bytes,
    # (to find a physical spot to damage) we'll need to search for
    # the rot13 encrypted string.
    def moreinit(self):
        super().moreinit()
        self.uniquebytes = self.rot13(self.uniquebytes.decode()).encode()

    # overrides test_salvage.damage.
    #def damage(self, tablename):
    #    self.damage_inner(tablename, self.rot13(self.unique).encode())

if __name__ == '__main__':
    wttest.run()
