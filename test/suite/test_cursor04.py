#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base03.py
# 	Cursor operations
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest

class test_cursor04(wttest.WiredTigerTestCase):
    """
    Test cursor search and search_near
    """
    table_name1 = 'test_cursor04'
    nentries = 20

    scenarios = [
        ('row', dict(tablekind='row')),
        ('col', dict(tablekind='col')),
        ('fix', dict(tablekind='fix'))
        ]

    def config_string(self):
        """
        Return any additional configuration.
        This method may be overridden.
        """
        return ''

    def session_create(self, name, args):
        """
        session.create, but report errors more completely
        """
        try:
            self.session.create(name, args)
        except:
            print('**** ERROR in session.create("' + name + '","' + args + '") ***** ')
            raise

    def create_session_and_cursor(self):
        tablearg = "table:" + self.table_name1
        if self.tablekind == 'row':
            keyformat = 'key_format=S'
        else:
            keyformat = 'key_format=r'  # record format
        if self.tablekind == 'fix':
            valformat = 'value_format=8t'
        else:
            valformat = 'value_format=S'
        create_args = keyformat + ',' + valformat + self.config_string()
        print('creating session: ' + create_args)
        self.session_create(tablearg, create_args)
        self.pr('creating cursor')
        return self.session.open_cursor(tablearg, None, None)

    def genkey(self, i):
        if self.tablekind == 'row':
            return 'key' + str(i).zfill(5)  # return key00001, key00002, etc.
        else:
            return long(i+1)

    def genvalue(self, i):
        if self.tablekind == 'fix':
            return int(i & 0xff)
        else:
            return 'value' + str(i)

    def expect_either(self, cursor, lt, gt):
        origkey = cursor.get_key()
        near_ret = cursor.search_near()
        self.assertEqual(near_ret[0], 0)
        direction = near_ret[1]

        # Deletions for 'fix' clear the value, they
        # do not remove the key, so we expect '0' direction
        # (that is key found) for fix.
        if self.tablekind != 'fix':
            self.assertTrue(direction == 1 or direction == -1)
        else:
            self.assertEqual(direction, 0)

        if direction == 1:
            self.assertEqual(cursor.get_key(), self.genkey(gt))
            self.assertEqual(cursor.get_value(), self.genvalue(gt))
        elif direction == -1:
            self.assertEqual(cursor.get_key(), self.genkey(lt))
            self.assertEqual(cursor.get_value(), self.genvalue(lt))
        else:
            self.assertEqual(direction, 0)
            self.assertEqual(cursor.get_key(), origkey)
            self.assertEqual(cursor.get_value(), 0)
        
    def test_searches(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        cursor = self.create_session_and_cursor()

        # Some tests below expect keys between 0-10 to be available
        self.assertTrue(self.nentries > 10)

        # 0. Populate the key space
        for i in range(0, self.nentries):
            cursor.set_key(self.genkey(i))
            cursor.set_value(self.genvalue(i))
            cursor.insert()

        # 1. Calling search for a value that exists
        cursor.set_key(self.genkey(5))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_key(), self.genkey(5))
        self.assertEqual(cursor.get_value(), self.genvalue(5))

        # 2. Calling search for a value that does not exist
        cursor.set_key(self.genkey(self.nentries))
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # The key/value should be cleared on NOTFOUND
        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)

        # 2. Calling search_near for a value beyond the end
        cursor.set_key(self.genkey(self.nentries))
        near_ret = cursor.search_near()
        self.assertEqual(near_ret[0], 0)
        self.assertEqual(near_ret[1], -1)
        self.assertEqual(cursor.get_key(), self.genkey(self.nentries-1))
        self.assertEqual(cursor.get_value(), self.genvalue(self.nentries-1))

        # 2.a calling search_near for an existing value
        cursor.set_key(self.genkey(7))
        near_ret = cursor.search_near()
        self.assertEqual(near_ret[0], 0)
        self.assertEqual(near_ret[1], 0)
        self.assertEqual(cursor.get_key(), self.genkey(7))
        self.assertEqual(cursor.get_value(), self.genvalue(7))

        # 3. Delete some keys
        # Deletions for 'fix' clear the value, they
        # do not remove the key
        cursor.set_key(self.genkey(0))
        cursor.remove()
        cursor.set_key(self.genkey(5))
        cursor.remove()
        cursor.set_key(self.genkey(9))
        cursor.remove()
        cursor.set_key(self.genkey(10))
        cursor.remove()

        #cursor.reset()
        #for key, value in cursor:
        #    print('key: ' + str(key))
        #    print('value: ' + str(value))

        cursor.set_key(self.genkey(0))
        near_ret = cursor.search_near()
        self.assertEqual(near_ret[0], 0)
        if self.tablekind != 'fix':
            self.assertEqual(near_ret[1], 1)
            self.assertEqual(cursor.get_key(), self.genkey(1))
            self.assertEqual(cursor.get_value(), self.genvalue(1))
        else:
            self.assertEqual(near_ret[1], 0)
            self.assertEqual(cursor.get_key(), self.genkey(0))
            self.assertEqual(cursor.get_value(), 0)
            
        cursor.set_key(self.genkey(5))
        self.expect_either(cursor, 4, 6)

        cursor.set_key(self.genkey(9))
        self.expect_either(cursor, 8, 11)

        cursor.set_key(self.genkey(10))
        self.expect_either(cursor, 8, 11)

        cursor.close(None)

if __name__ == '__main__':
    wttest.run()
