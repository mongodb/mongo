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

import string, os
from suite_subprocess import suite_subprocess
import wiredtiger, wttest

# test_util01.py
#    Utilities: wt dump, as well as the dump cursor
class test_util01(wttest.WiredTigerTestCase, suite_subprocess):
    """
    Test wt dump.  We check for specific output.
    Note that we don't test dumping {key,value}_format that are integer
    here.  That's because the integer values are encoded and we don't
    want to duplicate the encoding/decoding algorithms.  Integer dump
    is tested implicity by test_util02 (which loads dumps created
    in various ways).
    """

    tablename = 'test_util01.a'
    nentries = 1000
    stringclass = ''.__class__

    def compare_config(self, expected_cfg, actual_cfg):
        # Replace '(' characters so configuration groups don't break parsing.
        # If we ever want to look for config groups this will need to change.
        da = dict(kv.split('=') for kv in
            actual_cfg.strip().replace('(',',').split(','))
        de = da.copy()
        de.update(kv.split('=') for kv in expected_cfg.strip().split(','))
        return da == de

    def compare_files(self, filename1, filename2):
        inheader = isconfig = False
        for l1, l2 in zip(open(filename1, "rb"), open(filename2, "rb")):
            if isconfig:
                if not self.compare_config(l1, l2):
                    return False
            elif l1 != l2:
                return False
            if inheader:
                isconfig = not isconfig
            if l1.strip() == 'Header':
                inheader = True
            if l1.strip() == 'Data':
                inheader = isconfig = False
        return True

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
        return ("%0.6d" % i) + ':' + self.get_string(i, 20)

    def get_value(self, i):
        return self.get_string(i, 1000)

    def dumpstr(self, s, hexoutput):
        """
        Return a key or value string formatted just as 'wt dump' would.
        Most printable characters (except tab, newline,...) are printed
        as is, otherwise, backslash hex is used.
        """
        result = ''
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
        return 'key_format=S,value_format=S'

    def dump(self, usingapi, hexoutput):
        params = self.table_config()
        self.session.create('table:' + self.tablename, params)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        ver = wiredtiger.wiredtiger_version()
        verstring = str(ver[1]) + '.' + str(ver[2]) + '.' + str(ver[3])
        with open("expect.out", "w") as expectout:
            if not usingapi:
                # Note: this output is sensitive to the precise output format
                # generated by wt dump.  If this is likely to change, we should
                # make this test more accommodating.
                expectout.write('WiredTiger Dump (WiredTiger Version ' + verstring + ')\n')
                if hexoutput:
                    expectout.write('Format=hex\n')
                else:
                    expectout.write('Format=print\n')
                expectout.write('Header\n')
                expectout.write('table:' + self.tablename + '\n')
                expectout.write('colgroups=,columns=,' + params + '\n')
                expectout.write('Data\n')
            for i in range(0, self.nentries):
                key = self.get_key(i)
                value = self.get_value(i)
                cursor[key] = value
                expectout.write(self.dumpstr(key, hexoutput))
                expectout.write(self.dumpstr(value, hexoutput))
        cursor.close()

        self.pr('calling dump')
        with open("dump.out", "w") as dumpout:
            if usingapi:
                if hexoutput:
                    dumpopt = "dump=hex"
                else:
                    dumpopt = "dump=print"
                dumpcurs = self.session.open_cursor('table:' + self.tablename,
                                                    None, dumpopt)
                for key, val in dumpcurs:
                    dumpout.write(str(key) + "\n" + str(val) + "\n")
                dumpcurs.close()
            else:
                dumpargs = ["dump"]
                if hexoutput:
                    dumpargs.append("-x")
                dumpargs.append(self.tablename)
                self.runWt(dumpargs, outfilename="dump.out")

        self.assertTrue(self.compare_files("expect.out", "dump.out"))

    def test_dump_process(self):
        self.dump(False, False)

    def test_dump_process_hex(self):
        self.dump(False, True)

    def test_dump_api(self):
        self.dump(True, False)

    def test_dump_api_hex(self):
        self.dump(True, True)


if __name__ == '__main__':
    wttest.run()
