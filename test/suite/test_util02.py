#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util02.py
# 	Utilities: wt load
#

import unittest
import wiredtiger
import wttest
from suite_subprocess import suite_subprocess
import os
import string

class test_util02(wttest.WiredTigerTestCase, suite_subprocess):
    """
    Test wt load
    """

    tablename = 'test_util02.a'
    tablename2 = 'test_util02.b'
    nentries = 1000
    stringclass = ''.__class__

    scenarios = [
        ('SS', dict(key_format='S',value_format='S')),
        ('rS', dict(key_format='r',value_format='S')),
        ('ri', dict(key_format='r',value_format='i')),
        ('ii', dict(key_format='i',value_format='i')),
        ]

    # python has a filecmp.cmp function, but different versions
    # of python approach file comparison differently.  To make
    # sure we really get byte for byte comparison, we define it here.

    def compare_files(self, filename1, filename2):
        bufsize = 4096
        if os.path.getsize(filename1) != os.path.getsize(filename2):
            print filename1 + ' size = ' + str(os.path.getsize(filename1))
            print filename2 + ' size = ' + str(os.path.getsize(filename2))
            return False
        with open(filename1, "rb") as fp1:
            with open(filename2, "rb") as fp2:
                while True:
                    b1 = fp1.read(bufsize)
                    b2 = fp2.read(bufsize)
                    if b1 != b2:
                        return False
                    # files are identical size
                    if not b1:
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
            key = self.get_key(i)
            value = self.get_value(i)
            cursor.set_key(key)
            cursor.set_value(value)
            cursor.insert()
        cursor.close()

        dumpargs = ["dump"]
        if hexoutput:
            dumpargs.append("-x")
        dumpargs.append(self.tablename)
        self.runWt(dumpargs, outfilename="dump.out")

        # Create a placeholder for the new table.
        self.session.create('table:' + self.tablename2, params)

        # TODO: this shouldn't be needed.
        # The output of 'wt dump' includes 'colgroups=' and 'columns='
        # which are not acceptable to 'wt load', so we need to patch that.
        #with open("dump.out", "r+") as f:
        #    old = f.read()
        #    f.seek(0)
        #    new = old.replace("colgroups=,columns=,", "colgroups=(),columns=(),")
        #    f.write(new)
        # end TODO

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
