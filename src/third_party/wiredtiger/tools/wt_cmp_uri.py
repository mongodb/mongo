#!/usr/bin/env python3
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

# Compare WT data files in two different home directories.

import os, sys
from wt_tools_common import wiredtiger_open
from wiredtiger import WT_NOTFOUND, wiredtiger_strerror

def usage_exit():
    print('Usage: wt_cmp_uri.py [ -t timestamp ] [ -v ] dir1/uri1 [ -t timestamp2 ] dir2/uri2')
    print('  dir1 and dir2 are POSIX pathnames to WiredTiger home directories,')
    print('  uri1 and uri2 are WiredTiger URIs, e.g. table:foo or file:abc.wt')
    print('Options:')
    print('  -v      verbose - shows every key/value')
    print('  -t ts   read at a timestamp (a different timestamp for each dir may be specified)')
    sys.exit(1)

verboseFlag = False
def verbose(s):
    if verboseFlag:
        print("VERBOSE>>> " + s)

def die(reason):
    print('wt_cmp_uri: error: ' + reason, file=sys.stderr)
    sys.exit(1)

# An encapsulation of a WT cursor
class CompareCursor:
    def __init__(self, cursor, uri, name, reverse):
        self.cursor = cursor
        self.uri = uri
        self.name = name
        self.reverse = reverse
        self.at_end = False

    def close(self):
        session = self.cursor.session
        self.cursor.close()
        session.close()

    # Return 0 or WT_NOTFOUND.  If WT_NOTFOUND,
    # ensure that all future calls return the same.
    # If any other error, fail and exit.
    def cursor_next(self):
        if self.at_end:
            return WT_NOTFOUND
        ret = self.cursor.next()
        self.at_end = (ret == WT_NOTFOUND)
        if ret != 0 and ret != WT_NOTFOUND:
            die('{}: cursor operation failed: {}'.format(self.name, wiredtiger_strerror(ret)))
        return ret

# Open a cursor at a timestamp
def wt_open(conn, timestamp, uri):
    session = conn.open_session()
    if timestamp != None:
        config = 'read_timestamp=' + timestamp
        verbose('begin_transaction config: ' + config)
        session.begin_transaction(config)
    return session.open_cursor(uri, None, 'readonly')

# Find a uri in the metadata and determine if it uses the 'reverse' collator.
# test/format may use this collator.
def is_reverse(session, uri):
    mc = session.open_cursor('metadata:')
    config = mc[uri]
    mc.close()
    return 'collator=reverse' in config

# Get an encapsulation of a WT cursor at the given timestamp.
def get_compare_cursor(conn, timestamp, uri, arg):
    cursor = wt_open(conn, timestamp, uri)
    reverse = is_reverse(cursor.session, uri)
    return CompareCursor(cursor, uri, arg, reverse)

# Show a limited length version of a key/value
def show_kv(kv):
    s = str(kv)
    if len(s) > 100:
        s = s[0:100] + '...'
    return s

# Walk through both cursors, comparing them.  Generally, it's very easy, when the
# keys don't compare equally, we would know to do 'next' on the smaller key.  Except
# when custom collation is behind, we don't really know how to 'recover' in the general case.
# test/format uses custom collation, but only with a "reverse" collator - if we detect keys
# are going in reverse, we take note of that.
# 
def compare_cursors(cc1, cc2, version):
    nrecords = 0
    verbose('{}: next'.format(cc1.name))
    if cc1.reverse != cc2.reverse:
        print('ERROR: different collators for URIs {} and {}'.format(cc1.name, cc2.name))
        return 1
    reverse = cc1.reverse

    ecode = 0
    while cc1.cursor_next() == 0:
        k1 = cc1.cursor.get_key()
        v1 = cc1.cursor.get_value()
        verbose('  key: {} value: {}'.format(show_kv(k1), show_kv(v1)))

        verbose('{}: next'.format(cc2.name))
        if cc2.cursor_next() != 0:
            print('EOF on {}'.format(cc2.name))
            return 1
        nrecords += 1
        k2 = cc2.cursor.get_key()
        v2 = cc2.cursor.get_value()
        verbose('  key: {} value: {}'.format(show_kv(k2), show_kv(v2)))

        missing = 0
        while k1 != k2:
            ecode = 1
            if (not reverse and k1 < k2) or (reverse and k2 < k1):
                # Keep a running total of how many missing from one table or another,
                # so we don't show a huge number that are missing.
                if missing > 0:
                    if missing >= 10:
                        print(' total of {} missing entries here'.format(missing))
                    missing = 0
                if missing <= 0:
                    missing -= 1
                if missing > -10:
                    print('missing key={} on {}'.format(show_kv(k1), cc2.name))
                elif missing == -10:
                    print(' ...')
                verbose('{}: next'.format(cc1.name))
                if cc1.cursor_next() != 0:
                    if abs(missing) >= 10:
                        print(' total of {} missing entries here'.format(abs(missing)))
                    print('EOF on {}'.format(cc1.name))
                    print('{} records read'.format(nrecords))
                    return 1
                nrecords += 1
                k1 = cc1.cursor.get_key()
                v1 = cc1.cursor.get_value()
                verbose('  key: {} value: {}'.format(show_kv(k1), show_kv(v1)))
            else:
                # Keep a running total of how many missing from one table or another,
                # so we don't show a huge number that are missing.
                if missing < 0:
                    if missing <= -10:
                        print(' total of {} missing entries here'.format(abs(missing)))
                    missing = 0
                if missing >= 0:
                    missing += 1
                if missing < 10:
                    print('missing key={} on {}'.format(show_kv(k2), cc1.name))
                elif missing == 10:
                    print(' ...')
                verbose('{}: next'.format(cc2.name))
                if cc2.cursor_next() != 0:
                    if abs(missing) >= 10:
                        print(' total of {} missing entries here'.format(abs(missing)))
                    print('EOF on {}'.format(cc2.name))
                    print('{} records read'.format(nrecords))
                    return 1
                nrecords += 1
                k2 = cc2.cursor.get_key()
                v2 = cc2.cursor.get_value()
                verbose('  key: {} value: {}'.format(show_kv(k2), show_kv(v2)))

        if abs(missing) >= 10:
            print(' total of {} missing entries here'.format(abs(missing)))
            
        # At this point, the keys are the same
        if v1 != v2:
            print('for key={}, value is different: {}: {}, {}: {}'.format(show_kv(k1), cc1.name, show_kv(v1), cc2.name, show_kv(v2)))
            ecode = 1
        elif version:
            # If we want to use the version cursor:
            vcur1 = cc1.cursor.session.open_cursor(cc1.uri, None, 'debug=(dump_version)')
            vcur1.set_key(k1)
            if vcur1.search() != 0:
                print('unexpected version cursor search')
                ecode = 1
                return
            vcur2 = cc2.cursor.session.open_cursor(cc2.uri, None, 'debug=(dump_version)')
            vcur2.set_key(k2)
            if vcur2.search() != 0:
                print('unexpected version cursor search')
                ecode = 1
                return
            vcc1 = CompareCursor(vcur1, cc1.uri, 'version(key={}) for {}'.format(k1, cc1.name))
            vcc2 = CompareCursor(vcur2, cc2.uri, 'version(key={}) for {}'.format(k2, cc2.name))
            verbose('READY for version cursor')
            ver_ecode = compare_version_cursors(vcc1, vcc2)
            vcur1.close()
            vcur2.close()
            if ver_code != 0:
                ecode = 1
        verbose('{}: next'.format(cc1.name))

    if cc2.cursor_next() == 0:
        print('EOF on {}'.format(cc1.name))
        ecode = 1
    print('{} records read'.format(nrecords))
    return ecode

# Deeply compare two keys at all known timestamps using the version cursor.
# Note: this function is not yet used.
def compare_version_cursors(cc1, cc2):
    nrecords = 0
    ret1 = 0
    ret2 = 0
    ecode = 0
    while ret1 == 0:
        if ret2 != 0:
            print('EOF on {}'.format(cc2.name))
            ecode = 1
            break
        nrecords += 1
        k1 = cc1.cursor.get_key()
        v1 = cc1.cursor.get_value()
        k2 = cc2.cursor.get_key()
        v2 = cc2.cursor.get_value()
        verbose('Version: GOT: {}:{} {}:{}'.format(k1,v1,k2,v2))
        while k1 != k2:
            ecode = 1
            if k1 < k2:
                print('missing key={} on {}'.format(str(k1), cc2.name))
                if cc1.cursor_next() != 0:
                    print('EOF on {}'.format(cc1.name))
                    return 1
                nrecords += 1
                k1 = cc1.cursor.get_key()
                v1 = cc1.cursor.get_value()
            else:
                print('missing key={} on {}'.format(str(k2), cc1.name))
                if cc2.cursor_next() != 0:
                    print('EOF on {}'.format(cc2.name))
                    return 1
                nrecords += 1
                k2 = cc2.cursor.get_key()
                v2 = cc2.cursor.get_value()

        # At this point, the keys are the same
        if v1 != v2:
            print('for key={}, value is different: {}: {}, {}: {}'.format(k1, cc1.name, v1, cc2.name, v2))
            ecode = 1
        verbose('Version: finished compare')

        ret1 = cc1.cursor_next()
        ret2 = cc2.cursor_next()

    # At this point, ret1 is not 0, that is, we've reached the end of data for
    # the first table.  If we have a zero return on the second table, there's still
    # more data.
    if ret2 == 0:
        print('EOF on {}'.format(cc1.name))
        ecode = 1
    print('{} records read on version cursor'.format(nrecords))
    return ecode

def get_dir_uri(name):
    slash = name.rindex('/')
    return (name[0:slash], name[slash+1:])

def wiredtiger_compare_uri(args):
    timestamp1 = None
    timestamp2 = None

    while len(args) > 1 and args[0][0] == '-':
        if args[0] == '-v':
            global verboseFlag
            verboseFlag = True
            args = args[1:]
        elif args[0] == '-t':
            timestamp1 = args[1]
            timestamp2 = args[1]
            args = args[2:]
        else:
            usage_exit()

    if len(args) < 2:
        usage_exit()
    arg1 = args[0]
    (wtdir1, uri1) = get_dir_uri(arg1)
    args = args[1:]

    # Look for a second -t argument.
    while len(args) > 1 and args[0][0] == '-':
        if args[0] == '-t':
            timestamp2 = args[1]
            args = args[2:]

    if len(args) != 1:
        usage_exit()
    arg2 = args[0]
    (wtdir2, uri2) = get_dir_uri(arg2)

    # We do allow wtdir1 and wtdir2 to be the same, it is useful to compare at different
    # timestamps. But when they are the same, we must use the same connection.

    verbose('wiredtiger_open({})'.format(wtdir1))
    conn1 = wiredtiger_open(wtdir1, 'readonly')

    if wtdir1 == wtdir2:
        conn2 = conn1
    else:
        verbose('wiredtiger_open({})'.format(wtdir2))
        conn2 = wiredtiger_open(wtdir2, 'readonly')

    try:
        cc1 = get_compare_cursor(conn1, timestamp1, uri1, arg1)
    except:
        print('Failed opening {} in {} at timestamp {}'.format(uri1, wtdir1, timestamp1))
        raise
    try:
        cc2 = get_compare_cursor(conn2, timestamp2, uri2, arg2)
    except:
        print('Failed opening {} in {} at timestamp {}'.format(uri2, wtdir2, timestamp2))
        raise

    # Not running with version cursors, it's not quite ready for prime time.
    ecode = compare_cursors(cc1, cc2, False)

    cc1.close()
    cc2.close()
    conn1.close()
    if conn2 != conn1:
        conn2.close()

    sys.exit(ecode)

if __name__ == "__main__":
    wiredtiger_compare_uri(sys.argv[1:])
