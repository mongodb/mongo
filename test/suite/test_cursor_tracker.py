#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_cursor_tracker.py
#	Tracker for testing cursor operations.  Keys and values
#	are generated automatically based somewhat on position,
#	and are stored simultaneously in the WT table and
#	in a compact representation in python data structures
#	(self.bitlist, self.vers).  A set of convenience functions
#	allows us to insert/remove/update keys on a cursor,
#	navigate forward, back, etc. and verify K/V pairs in
#	the table.  Comprehensive tests can then be built up.
#
#	All keys and values are generated, based on a triple:
#	[major, minor, version].  The key generator is a pure function
#	K that returns a string and takes inputs (major, minor).
#	K is implemented as encode_key() below, its inverse is decode_key().
#	The value generator is a pure function V that returns a string
#	based on inputs (major, minor, version).  It is implemented
#	as encode_value(), its inverse being decode_value().
#
#	When a test starts, calling cur_initial_conditions, a set
#	of N K/V populates the table.  These correspond to major
#	numbers.  For example, with N==3, major/minor numbers of
#	[0,0], [0,1], [0,2] are inserted into the table.
#	The table is then closed (and session closed) to guarantee
#	that the associated data is written to disk before continuing.
#	The theory is that since changes in WT are stored in skip lists,
#	we want the test to be aware of preexisting values (those having
#	minor number == 0) so we can try all combinations of adding
#	new skip list entries, and removing/updating both skip list
#	and original values.
#
#	After initial conditions are set up, the test calls functions
#	such as cur_insert to insert values.  Since minor numbers
#	sort secondarily to major, they will take logical positions
#	in specific places relative to major (preexisting) K/V pairs.
#
#	Updating an existing value is detected in the python data
#	structures, and result in incrementing the version number.
#	Thus, the key remains unchanged, but the associated value
#	changes (the size of the value may be altered as well).

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest

class TestCursorTracker(wttest.WiredTigerTestCase):
    table_name1 = 'test_cursor'

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

    def table_dump(self, name):
        cursor = self.session.open_cursor('table:' + name, None, None)
        self._dumpcursor(cursor)
        cursor.close(None)

    def __init__(self, testname):
        wttest.WiredTigerTestCase.__init__(self, testname)
        self.cur_initial_conditions(None, 0, 'row')

    def setup_encoders_decoders(self):
        if self.tablekind == 'row':
            self.encode_key = self.encode_key_row
            self.encode_value = self.encode_value_row
            self.decode_key = self.decode_key_row
            self.decode_value = self.decode_value_row
        elif self.tablekind == 'col':
            self.encode_key = self.encode_key_col
            self.encode_value = self.encode_value_col
            self.decode_key = self.decode_key_col
            self.decode_value = self.decode_value_col
        else:
            self.encode_key = self.encode_key_fix
            self.encode_value = self.encode_value_fix
            self.decode_key = self.decode_key_fix
            self.decode_value = self.decode_value_fix

    def cur_initial_conditions(self, cursor, npairs, tablekind):
        if npairs >= 0xffff:
            raise Exception('cur_initial_conditions: npairs too big')
        self.tablekind = tablekind
        self.setup_encoders_decoders()
        self.bitlist = [(x << 32) for x in range(npairs)]
        self.vers = dict((x << 32, 0) for x in range(npairs))
        self.nopos = True  # not positioned on a valid element
        self.curpos = -1
        self.curbits = 0xffffffffffff
        self.curremoved = False # K/V data in cursor does not correspond to active data
        # TODO: sepearated only for debugging
        if self.tablekind != 'row':
            for i in range(npairs):
                print ('typebefore: ' + str(type((int(self.encode_key(i << 32))))))
                cursor.set_key(int(self.encode_key(i << 32)))
                cursor.set_value(self.encode_value(i << 32))
                cursor.insert()
        else:
            for i in range(npairs):
                cursor.set_key(self.encode_key(i << 32))
                cursor.set_value(self.encode_value(i << 32))
                cursor.insert()
	# TODO: close, reopen the session!

    def bits_to_triple(self, bits):
        major = (bits >> 32) & 0xffff
        minor = (bits >> 16) & 0xffff
        version = (bits) & 0xffff
        return [major, minor, version]

    def triple_to_bits(self, major, minor, version):
        return ((major & 0xffff) << 32) | ((minor & 0xffff) << 16) | (version & 0xffff)

    def key_to_bits(self, key):
        pass

    # There are variants of encode_key for each table kind: 'row', 'col', 'fix'

    ################   ROW   ################
    # TODO: something more sophisticated
    def encode_key_row(self, bits):
        # Prepend 0's to make the string exactly len 16
        result = str(bits)
        result = '0000000000000000'[len(result):] + result
        return result

    # TODO: something more sophisticated
    def encode_value_row(self, bits):
        return self.encode_key(bits)

    def decode_key_row(self, s):
        return int(s)

    def decode_value_row(self, s):
        return int(s)

    ################   COL   ################
    def encode_key_col(self, bits):
        # Prepend 0's to make the string exactly len 16
        maj = ((bits << 32) & 0xffff) + 1
        min = (bits << 16) & 0xffff
        return int((maj << 16) | min)

    # TODO: something more sophisticated
    def encode_value_col(self, bits):
        return self.encode_key_row(bits)

    def decode_key_col(self, s):
        maj = ((bits << 16) & 0xffff) - 1
        min = bits & ffff
        return ((maj << 32) | (min << 16))

    def decode_value_col(self, s):
        return int(s)

    ################   FIX   ################
    def encode_key_fix(self, bits):
        # Prepend 0's to make the string exactly len 16
        maj = ((bits << 32) & 0xffff) + 1
        min = (bits << 16) & 0xffff
        return int((maj << 16) | min)

    def encode_value_fix(self, bits):
        # TODO: only 8 bits
        return self.encode_key_row(bits) & 0xff

    def decode_key_fix(self, s):
        maj = ((bits << 16) & 0xffff) - 1
        min = bits & ffff
        return ((maj << 32) | (min << 16))

    def decode_value_fix(self, s):
        # TODO: only 8 bits
        return int(s)

    def setpos(self, newpos):
        length = len(self.bitlist)
        if newpos < 0 or newpos >= length:
            self.curpos = -1
            self.nopos = True
            self.curremoved = False
            self.curbits = 0xffffffffffff
            return False
        else:
            self.curpos = newpos
            self.nopos = False
            self.curremoved = False
            self.curbits = self.bitlist[newpos]
            return True

    def cur_first(self, cursor):
        self.setpos(0)
        cursor.first()
        self.curremoved = False

    def cur_last(self, cursor):
        self.setpos(len(self.bitlist) - 1)
        cursor.last()
        self.curremoved = False

    def cur_update(self, cursor, key):
        # TODO:
        pass

    def cur_insert(self, cursor, major, minor):
        bits = self.triple_to_bits(major, minor, 0)
        if bits not in self.vers:
            self.bitlist.append(bits)
            #TODO: why doesn't self.bitlist.sort() work?
            self.bitlist = sorted(self.bitlist)
            self.vers[bits] = 0
        else:
            raise Exception('cur_insert: key already exists: ' + str(major) + ',' + str(minor))
        pos = next(i for i in xrange(len(self.bitlist)) if self.bitlist[i] == bits)
        self.setpos(pos)
        cursor.set_key(self.encode_key(bits))
        cursor.set_value(self.encode_value(bits))
        cursor.insert()

    def cur_remove_here(self, cursor):
        # TODO: handle the exception case
        if self.nopos:
            expectException = True
        else:
            expectException = False
            del self.vers[self.curbits & 0xffffffff0000]
            self.bitlist.pop(self.curpos)
            self.setpos(self.curpos - 1)
            self.nopos = True
            self.curremoved = True
        cursor.remove()

    def cur_remove(self, cursor, major, minor):
        # TODO:
        raise Exception('cur_remove not yet coded')
        bits = self.triple_to_bits(major, minor, 0)
        if bits in self.vers:
            cursor.set_key(self.encode_key(key))
            cursor.set_value(self.encode_value(key))
            cursor.remove()
        else:
            raise Exception('cur_remove: key does not exist: ' + str(major) + ',' + str(minor))

    def check_cursor_ret(self, ret, want):
        if ret != want:
            if ret == 0:
                self.fail('cursor did not return NOTFOUND')
            elif ret == wiredtiger.WT_NOTFOUND:
                self.fail('cursor returns NOTFOUND unexpectedly')
            else:
                self.fail('unexpected return from cursor: ' + ret)

    def cur_check_forward(self, cursor, n):
        if n < 0:
            n = len(self.bitlist)
        for i in range(n):
            self.cur_next(cursor)
            self.cur_check_here(cursor)

    def cur_next(self, cursor):
        # Note: asymmetric with cur_previous, nopos corresponds to 'half'
        if self.setpos(self.curpos + 1):
            bitsret = 0
        else:
            bitsret = wiredtiger.WT_NOTFOUND
        self.check_cursor_ret(cursor.next(), bitsret)

    def cur_check_backward(self, cursor, n):
        if n < 0:
            n = len(self.bitlist)
        for i in range(n):
            self.cur_previous(cursor)
            self.cur_check_here(cursor)

    def cur_previous(self, cursor):
        if self.nopos:
            pos = self.curpos
        else:
            pos = self.curpos - 1
        if self.setpos(pos):
            bitsret = 0
        else:
            bitsret = wiredtiger.WT_NOTFOUND
        self.check_cursor_ret(cursor.prev(), bitsret)

    def cur_check_here(self, cursor):
        # Cannot check immediately after a remove, since the K/V in the cursor
        # does not correspond to anything
        if self.curremoved:
            raise Exception('cur_check_here: cursor.get_key, get_value are not valid')
        elif self.nopos:
            self.assertRaises(WiredTigerError, cursor.get_key)
            self.assertRaises(WiredTigerError, cursor.get_value)
        else:
            bits = self.curbits
            self.cur_check(cursor, cursor.get_key(), self.encode_key(bits), True)
            self.cur_check(cursor, cursor.get_value(), self.encode_value(bits), False)

    def dumpbitlist(self):
        print('bits array:')
        for bits in self.bitlist:
            print('  ' + str(self.bits_to_triple(bits)) + ' = ' + str(bits))

    def _cursor_key_to_string(self, k):
            return str(self.bits_to_triple(self.decode_key(k))) + ' = ' + k

    def _cursor_value_to_string(self, v):
            return str(self.bits_to_triple(self.decode_key(v))) + ' = ' + v

    def _dumpcursor(self, cursor):
        print('cursor')
        cursor.reset()
        for k,v in cursor:
            print('  ' + self._cursor_key_to_string(k) + ' ' +
                  self._cursor_value_to_string(v))

    def cur_dump_here(self, cursor, prefix):
        try:
            k = self._cursor_key_to_string(cursor.get_key())
        except:
            k = '[invalid]'
        try:
            v = self._cursor_value_to_string(cursor.get_value())
        except:
            v = '[invalid]'
        print(prefix + k + ' ' + v)
        
    def cur_check(self, cursor, got, want, iskey):
        if got != want:
            if iskey:
                goti = self.decode_key(got)
                wanti = self.decode_key(want)
            else:
                goti = self.decode_value(got)
                wanti = self.decode_value(want)
            gotstr = str(self.bits_to_triple(goti))
            wantstr = str(self.bits_to_triple(wanti))
            self.dumpbitlist()
            # Note: dumpcursor() resets the cursor position,
            # but we're about to issue a fatal error, so it's okay
            self._dumpcursor(cursor)
            if iskey:
                kind = 'key'
            else:
                kind = 'value'
            self.fail('mismatched ' + kind + ', want: ' + wantstr + ', got: ' + gotstr)


if __name__ == '__main__':
    wttest.run()
