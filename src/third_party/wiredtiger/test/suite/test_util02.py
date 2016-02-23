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

import string, os
import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtscenario import check_scenarios

# test_util02.py
#    Utilities: wt load
class test_util02(wttest.WiredTigerTestCase, suite_subprocess):
    """
    Test wt load
    """

    tablename = 'test_util02.a'
    tablename2 = 'test_util02.b'
    nentries = 1000
    stringclass = ''.__class__

    scenarios = check_scenarios([
        ('SS', dict(key_format='S',value_format='S')),
        ('rS', dict(key_format='r',value_format='S')),
        ('ri', dict(key_format='r',value_format='i')),
        ('ii', dict(key_format='i',value_format='i')),
    ])

    def get_string(self, i, len):
        """
        Return a pseudo-random, but predictable string that uses
        all characters.  As a special case, key 0 returns all characters
        1-255 repeated
        """
        ret = ''
        if i == 0:
            for j in range (0, len):
                # we ensure that there are no internal nulls, that would
                # truncate the string when we're using the 'S' encoding
                # The last char in a string is null anyway, so that's tested.
                ret += chr(j%255 + 1)
        else:
            for j in range(0, len / 3):
                k = i + j
                # no internal nulls...
                ret += chr(k%255 + 1) + chr((k*3)%255 + 1) + chr((k*7)%255 + 1)
        return ret

    def get_key(self, i):
        if self.key_format == 'S':
            return ("%0.6d" % i) + ':' + self.get_string(i, 20)
        elif self.key_format == 'r':
            return long(i + 1)
        else:
            return i + 1

    def get_value(self, i):
        if self.value_format == 'S':
            return self.get_string(i, 1000)
        else:
            # format is 'i' for this test
            # return numbers throughout the 2^64 range
            if i == 0:
                return 0
            else:
                if i < 64:
                    mask = (1 << i) - 1
                else:
                    mask = 0xffffffffffffffff
                # multiply by a large prime get pseudo random bits
                n = (i * 48112959837082048697) & mask
                if n & 0x8000000000000000 != 0:
                    n = n - 0x10000000000000000
                return n

    def dumpstr(self, obj, hexoutput):
        """
        Return a key or value string formatted just as 'wt dump' would.
        Most printable characters (except tab, newline,...) are printed
        as is, otherwise, backslash hex is used.
        """
        result = ''
        if type(obj) == self.stringclass:
            for c in s:
                if hexoutput:
                    result += "%0.2x" % ord(c)
                elif c == '\\':
                    result += '\\\\'
                elif c == ' ' or (c in string.printable and not c in string.whitespace):
                    result += c
                else:
                    result += '\\' + "%0.2x" % ord(c)
            if hexoutput:
                result += '00\n'
            else:
                result += '\\00\n'
        return result

    def table_config(self):
        return 'key_format=' + self.key_format + ',value_format=' + self.value_format

    def load_process(self, hexoutput):
        params = self.table_config()
        self.session.create('table:' + self.tablename, params)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        for i in range(0, self.nentries):
            cursor[self.get_key(i)] = self.get_value(i)
        cursor.close()

        dumpargs = ["dump"]
        if hexoutput:
            dumpargs.append("-x")
        dumpargs.append(self.tablename)
        self.runWt(dumpargs, outfilename="dump.out")

        # Create a placeholder for the new table.
        self.session.create('table:' + self.tablename2, params)

        self.runWt(["load", "-f", "dump.out", "-r", self.tablename2])

        cursor = self.session.open_cursor('table:' + self.tablename2, None, None)
        self.assertEqual(cursor.key_format, self.key_format)
        self.assertEqual(cursor.value_format, self.value_format)
        i = 0
        for key, val in cursor:
            self.assertEqual(key, self.get_key(i))
            self.assertEqual(val, self.get_value(i))
            i += 1
        cursor.close()

    def test_load_process(self):
        self.load_process(False)

    def test_load_process_hex(self):
        self.load_process(True)

if __name__ == '__main__':
    wttest.run()
