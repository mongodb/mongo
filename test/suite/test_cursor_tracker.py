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
#
# test_cursor_tracker.py
#       Tracker for testing cursor operations.  Keys and values
#       are generated automatically based somewhat on position,
#       and are stored simultaneously in the WT table and
#       in a compact representation in python data structures
#       (self.bitlist, self.vers).  A set of convenience functions
#       allows us to insert/remove/update keys on a cursor,
#       navigate forward, back, etc. and verify K/V pairs in
#       the table.  Comprehensive tests can then be built up.
#
#       All keys and values are generated, based on a triple:
#       [major, minor, version].  The key generator is a pure function
#       K that returns a string and takes inputs (major, minor).
#       K is implemented as encode_key() below, its inverse is decode_key().
#       The value generator is a pure function V that returns a string
#       based on inputs (major, minor, version).  It is implemented
#       as encode_value(), its inverse being decode_value().
#
#       When a test starts, calling cur_initial_conditions, a set
#       of N K/V populates the table.  These correspond to major
#       numbers.  For example, with N==3, major/minor numbers of
#       [0,0], [0,1], [0,2] are inserted into the table.
#       The table is then closed (and session closed) to guarantee
#       that the associated data is written to disk before continuing.
#       The theory is that since changes in WT are stored in skip lists,
#       we want the test to be aware of preexisting values (those having
#       minor number == 0) so we can try all combinations of adding
#       new skip list entries, and removing/updating both skip list
#       and original values.
#
#       After initial conditions are set up, the test calls functions
#       such as cur_insert to insert values.  Since minor numbers
#       sort secondarily to major, they will take logical positions
#       in specific places relative to major (preexisting) K/V pairs.
#
#       Updating an existing value is detected in the python data
#       structures, and result in incrementing the version number.
#       Thus, the key remains unchanged, but the associated value
#       changes (the size of the value may be altered as well).
#
#       TODO: we need to separate the cursor tracking information
#       (the current position where we believe we are) from
#       the database information (what we think is in the data storage).
#       Once that's done, we can have multiple cursor tests
#       (though simulating transactions would probably be beyond what
#       we want to do here).

import hashlib
import wiredtiger, wttest

class TestCursorTracker(wttest.WiredTigerTestCase):
    table_name1 = 'test_cursor'
    DELETED = 0xffffffffffffffff
    TRACE_API = False    # a print output for each WT API call

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
        cursor.close()

    def __init__(self, testname):
        wttest.WiredTigerTestCase.__init__(self, testname)
        self.cur_initial_conditions(None, 0, None, None, None)

    # traceapi and friends are used internally in this module
    def traceapi(self, s):
        if self.TRACE_API:
            print('> ' + s)

    def traceapi_before(self, s):
        if self.TRACE_API:
            print('> ' + s + '...')

    def traceapi_after(self, s):
        if self.TRACE_API:
            print('  ==> ' + s)

    def setup_encoders_decoders(self):
        if self.tablekind == 'row':
            self.encode_key = self.encode_key_row
            self.decode_key = self.decode_key_row
            self.encode_value = self.encode_value_row_or_col
            self.decode_value = self.decode_value_row_or_col
        elif self.tablekind == 'col':
            self.encode_key = self.encode_key_col_or_fix
            self.decode_key = self.decode_key_col_or_fix
            self.encode_value = self.encode_value_row_or_col
            self.decode_value = self.decode_value_row_or_col
        else:
            self.encode_key = self.encode_key_col_or_fix
            self.decode_key = self.decode_key_col_or_fix
            self.encode_value = self.encode_value_fix
            self.decode_value = self.decode_value_fix

    def cur_initial_conditions(self, tablename, npairs, tablekind, keysizes, valuesizes, uri="table"):
        if npairs >= 0xffffffff:
            raise Exception('cur_initial_conditions: npairs too big')
        self.tablekind = tablekind
        self.isrow = (tablekind == 'row')
        self.setup_encoders_decoders()
        self.bitlist = [(x << 32) for x in range(npairs)]
        self.vers = dict((x << 32, 0) for x in range(npairs))
        self.nopos = True  # not positioned on a valid element
        self.curpos = -1
        self.curbits = 0xffffffffffff
        self.curremoved = False # K/V data in cursor does not correspond to active data
        self.keysizes = keysizes
        self.valuesizes = valuesizes
        if tablekind != None:
            cursor = self.session.open_cursor(uri + ':' + tablename, None, 'append')
            for i in range(npairs):
                wtkey = self.encode_key(i << 32)
                wtval = self.encode_value(i << 32)
                self.traceapi('cursor[' + str(wtkey) + '] = ' + str(wtval))
                cursor[wtkey] = wtval
            cursor.close()
            self.pr('reopening the connection')
            self.conn.close()
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)

    def bits_to_triple(self, bits):
        major = (bits >> 32) & 0xffffffff
        minor = (bits >> 16) & 0xffff
        version = (bits) & 0xffff
        return [major, minor, version]

    def triple_to_bits(self, major, minor, version):
        return ((major & 0xffffffff) << 32) | ((minor & 0xffff) << 16) | (version & 0xffff)

    def key_to_bits(self, key):
        pass

    def stretch_content(self, s, sizes):
        result = s
        if sizes != None:
            sha224 = hashlib.sha224(s)
            md5 = sha224.digest()
            low = sizes[0] - len(s)
            if low < 0:
                low = 0
            high = sizes[1] - len(s)
            if high < 0:
                high = 0
            diff = high - low
            nextra = (ord(md5[4]) % (diff + 1)) + low
            extra = sha224.hexdigest()
            while len(extra) < nextra:
                extra = extra + extra
            result = s + extra[0:nextra]
        return result

    def check_content(self, s, sizes):
        if sizes != None:
            stretched = self.stretch_content(s[0:20], sizes)
            self.assertEquals(s, stretched)

    # There are variants of {encode,decode}_{key,value} to be
    # used with each table kind: 'row', 'col', 'fix'

    def encode_key_row(self, bits):
        # Prepend 0's to make the string exactly len 20
        # 64 bits fits in 20 decimal digits
        # Then, if we're configured to have a longer key
        # size, we'll append some additional length
        # that can be regenerated based on the first part of the key
        result = str(bits)
        result = '00000000000000000000'[len(result):] + result
        if self.keysizes != None:
            result = self.stretch_content(result, self.keysizes)
        return result

    def decode_key_row(self, s):
        self.check_content(s, self.keysizes)
        return int(s[0:20])

    def encode_value_row_or_col(self, bits):
        # We use the same scheme as key.  So the 20 digits will
        # be the same, but the last part may well be different
        # if the size configuration is different.
        result = str(bits)
        result = '00000000000000000000'[len(result):] + result
        if self.valuesizes != None:
            result = self.stretch_content(result, self.valuesizes)
        return result

    def decode_value_row_or_col(self, s):
        self.check_content(s, self.valuesizes)
        return int(s[0:20])

    def encode_key_col_or_fix(self, bits):
        # 64 bit key
        maj = ((bits >> 32) & 0xffffffff) + 1
        min = (bits >> 16) & 0xffff
        return long((maj << 16) | min)

    def decode_key_col_or_fix(self, bits):
        maj = ((bits << 16) & 0xffffffff) - 1
        min = bits & 0xffff
        return ((maj << 32) | (min << 16))

    def encode_value_fix(self, bits):
        # can only encode only 8 bits
        maj = ((bits >> 32) & 0xff)
        min = (bits >> 16) & 0xff
        return (maj ^ min)

    def decode_value_fix(self, s):
        return int(s)

    def setpos(self, newpos, isforward):
        length = len(self.bitlist)
        while newpos >= 0 and newpos < length:
            if not self.isrow and self.bitlist[newpos] == self.DELETED:
                if isforward:
                    newpos = newpos + 1
                else:
                    newpos = newpos - 1
            else:
                self.curpos = newpos
                self.nopos = False
                self.curremoved = False
                self.curbits = self.bitlist[newpos]
                return True
        if newpos < 0:
            self.curpos = -1
        else:
            self.curpos = length - 1
        self.nopos = True
        self.curremoved = False
        self.curbits = 0xffffffffffff
        return False

    def cur_first(self, cursor, expect=0):
        self.setpos(0, True)
        self.traceapi('cursor.first()')
        self.assertEquals(0, cursor.reset())
        self.assertEquals(expect, cursor.next())
        self.curremoved = False

    def cur_last(self, cursor, expect=0):
        self.setpos(len(self.bitlist) - 1, False)
        self.traceapi('cursor.last()')
        self.assertEquals(0, cursor.reset())
        self.assertEquals(expect, cursor.prev())
        self.curremoved = False

    def cur_update(self, cursor, key):
        # TODO:
        pass

    def bitspos(self, bits):
        list = self.bitlist
        return next(i for i in xrange(len(list)) if list[i] == bits)

    def cur_insert(self, cursor, major, minor):
        bits = self.triple_to_bits(major, minor, 0)
        if bits not in self.vers:
            self.bitlist.append(bits)
            if self.isrow:
                #TODO: why doesn't self.bitlist.sort() work?
                self.bitlist = sorted(self.bitlist)
            self.vers[bits] = 0
        else:
            raise Exception('cur_insert: key already exists: ' + str(major) + ',' + str(minor))
        pos = self.bitspos(bits)
        self.setpos(pos, True)
        wtkey = self.encode_key(bits)
        wtval = self.encode_value(bits)
        self.traceapi('cursor[' + str(wtkey) + '] = ' + str(wtval))
        cursor[wtkey] = wtval

    def cur_remove_here(self, cursor):
        # TODO: handle the exception case
        if self.nopos:
            expectException = True
        else:
            expectException = False
            del self.vers[self.curbits & 0xffffffff0000]
            if self.isrow:
                self.bitlist.pop(self.curpos)
                self.setpos(self.curpos - 1, True)
                self.nopos = True
            else:
                self.bitlist[self.curpos] = self.DELETED
            self.curremoved = True
        self.traceapi('cursor.remove()')
        cursor.remove()

    def cur_recno_search(self, cursor, recno):
        wtkey = long(recno)
        self.traceapi('cursor.set_key(' + str(wtkey) + ')')
        cursor.set_key(wtkey)
        if recno > 0 and recno <= len(self.bitlist):
            want = 0
        else:
            want = wiredtiger.WT_NOTFOUND
        self.traceapi('cursor.search()')
        self.check_cursor_ret(cursor.search(), want)

    def cur_search(self, cursor, major, minor):
        bits = self.triple_to_bits(major, minor, 0)
        wtkey = self.encode_key(bits)
        self.traceapi('cursor.set_key(' + str(wtkey) + ')')
        cursor.set_key(wtkey)
        if bits in self.vers:
            want = 0
        else:
            want = wiredtiger.WT_NOTFOUND
        self.traceapi('cursor.search()')
        self.check_cursor_ret(cursor.search(), want)

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
            if self.nopos:
                break

    def cur_next(self, cursor):
        # Note: asymmetric with cur_previous, nopos corresponds to 'half'
        if self.setpos(self.curpos + 1, True):
            wantret = 0
        else:
            wantret = wiredtiger.WT_NOTFOUND
        self.traceapi('cursor.next()')
        self.check_cursor_ret(cursor.next(), wantret)

    def cur_check_backward(self, cursor, n):
        if n < 0:
            n = len(self.bitlist)
        for i in range(n):
            self.cur_previous(cursor)
            self.cur_check_here(cursor)
            if self.nopos:
                break

    def cur_previous(self, cursor):
        if self.nopos:
            pos = self.curpos
        else:
            pos = self.curpos - 1
        if self.setpos(pos, False):
            wantret = 0
        else:
            wantret = wiredtiger.WT_NOTFOUND
        self.traceapi('cursor.prev()')
        self.check_cursor_ret(cursor.prev(), wantret)

    def cur_check_here(self, cursor):
        # Cannot check immediately after a remove, since the K/V in the cursor
        # does not correspond to anything
        keymsg = '/requires key be set/'
        valuemsg = '/requires value be set/'
        if self.curremoved:
            raise Exception('cur_check_here: cursor.get_key, get_value are not valid')
        elif self.nopos:
            self.traceapi_before('cursor.get_key()')
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                cursor.get_key, keymsg)
            self.traceapi_after('<unknown>')
            self.traceapi_before('cursor.get_value()')
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                cursor.get_value, valuemsg)
            self.traceapi_after('<unknown>')
        else:
            bits = self.curbits
            self.traceapi_before('cursor.get_key()')
            wtkey = cursor.get_key()
            self.traceapi_after(str(wtkey))
            if self.isrow:
                self.cur_check(cursor, wtkey, self.encode_key(bits), True)
            else:
                self.cur_check(cursor, wtkey, self.bitspos(bits) + 1, True)
            self.traceapi_before('cursor.get_value()')
            wtval = cursor.get_value()
            self.traceapi_after(str(wtval))
            self.cur_check(cursor, wtval, self.encode_value(bits), False)

    def dumpbitlist(self):
        print('bits array:')
        for bits in self.bitlist:
            print('  ' + str(self.bits_to_triple(bits)) + ' = ' + str(bits))

    def _cursor_key_to_string(self, k):
            return str(self.bits_to_triple(self.decode_key(k))) + ' = ' + str(k)

    def _cursor_value_to_string(self, v):
            return str(self.bits_to_triple(self.decode_value(v))) + ' = ' + v

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
