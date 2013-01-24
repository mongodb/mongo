#
#
# Copyright (c) 2008-2013 WiredTiger, Inc.
#	All rights reserved.
#
# See the file LICENSE for redistribution information.
#
# WiredTiger public interface

'''
WiredTiger Python API.

This module exports several functions and classes.
'''

import struct
from urlparse import urlparse

from wiredtiger.service import WiredTiger
from service.ttypes import WT_RECORD
from thrift.transport import TSocket, TTransport
from thrift.protocol import TBinaryProtocol

def __wt2struct(fmt):
    if not fmt:
        return None, fmt
    # Big endian with no alignment is the default
    if fmt[0] in '@=<>!':
        tfmt = fmt[0]
        fmt = fmt[1:]
    else:
        tfmt = '>'
    return tfmt, fmt.replace('r', 'Q')

def unpack(fmt, s):
    tfmt, fmt = __wt2struct(fmt)
    if not fmt:
        return ()
    result = ()
    pfmt = tfmt
    sizebytes = 0
    for offset, f in enumerate(fmt):
        if f.isdigit():
            sizebytes += 1
        # With a fixed size, everything is encoded as a string
        if f in 'Su' and sizebytes > 0:
            f = 's'
        if f not in 'Su':
            pfmt += f
            sizebytes = 0
            continue

        # We've hit something that needs special handling, split any fixed-size
        # values we've already passed
        if len(pfmt) > 1:
            size = struct.calcsize(pfmt)
            result += struct.unpack_from(pfmt, s)
            s = s[size:]
        if f == 'S':
            l = s.find('\0')
            result += (s[:l],)
            s = s[l+1:]
        if f == 'u':
            if offset == len(fmt) - 1:
                result += (s,)
            else:
                l = struct.unpack_from(tfmt + 'l', s)[0]
                s = s[struct.calcsize(tfmt + 'l'):]
                result += (s[:l],)
                s = s[l:]
        pfmt = tfmt
        sizebytes = 0

    if len(pfmt) > 1:
        result += struct.unpack(pfmt, s)
    return result

def pack(fmt, *values):
    pfmt, fmt = __wt2struct(fmt)
    if not fmt:
        return ''
    i = sizebytes = 0
    for offset, f in enumerate(fmt):
        if f == 'S':
            # Note: this code is being careful about embedded NUL characters
            if sizebytes == 0:
                l = values[i].find('\0') + 1
                if not l:
                    l = len(values[i]) + 1
                pfmt += str(l)
                sizebytes = len(str(l))
            f = 's'
        elif f == 'u':
            if sizebytes == 0 and offset != len(fmt) - 1:
                l = len(values[i])
                pfmt += 'l' + str(l)
                values = values[:i] + (l,) + values[i:]
                sizebytes = len(str(l))
            f = 's'
        pfmt += f
        if f.isdigit():
            sizebytes += 1
            continue
        if f != 's' and sizebytes > 0:
            i += int(pfmt[-sizebytes:])
        else:
            i += 1
        sizebytes = 0
    return struct.pack(pfmt, *values)

class Cursor:
    def __init__(self, session, handle):
        self.session = session
        self.client = session.client
        self.id = handle.id
        self.keyfmt = handle.keyfmt
        self.valuefmt = handle.valuefmt

    def close(self, config=''):
        return self.client.close_cursor(self.id, config)

    def get_key(self):
        return unpack(self.keyfmt, self.key)

    def get_value(self):
        return unpack(self.keyfmt, self.value)

    def set_key(self, *args):
        self.key = pack(self.keyfmt, *args)

    def set_value(self, *args):
        self.value = pack(self.valuefmt, *args)

    def first(self):
        result = self.client.move_first(self.id)
        self.key, self.value = result.record.key, result.record.value
        return result.exact

    def last(self):
        result = self.client.move_last(self.id)
        self.key, self.value = result.record.key, result.record.value
        return result.exact

    def next(self):
        result = self.client.move_next(self.id)
        self.key, self.value = result.record.key, result.record.value
        return result.exact

    def prev(self):
        result = self.client.move_prev(self.id)
        self.key, self.value = result.record.key, result.record.value
        return result.exact

    def search(self):
        result = self.client.search(self.id, WT_RECORD(self.key, self.value))
        self.key, self.value = result.record.key, result.record.value
        return result.exact

    def insert(self):
        self.key = self.client.insert_record(self.id, WT_RECORD(self.key, self.value))

    def update(self):
        return self.client.update_record(self.id, self.value)

    def delete(self):
        return self.client.delete_record(self.id)


class Session:
    def __init__(self, conn, id):
        self.conn = conn
        self.client = conn.client
        self.id = id

    def close(self, config=''):
        self.client.close_session(self.id, config)

    def open_cursor(self, uri, config=''):
        return Cursor(self, self.client.open_cursor(self.id, uri, config))

    def dup_cursor(self, c, config=''):
        return Cursor(self, self.client.dup_cursor(self.id, c.id, config))

    def create_table(self, name, config=''):
        self.client.create_table(self.id, name, config)

    def rename_table(self, oldname, newname, config=''):
        self.client.rename_table(self.id, oldname, newname, config)

    def drop_table(self, name, config=''):
        self.client.drop_table(self.id, name, config)

    def truncate_table(self, name, start=None, end=None, config=''):
        self.client.truncate_table(self.id, name, name, start and start.id or 0, end and end.id or 0, config)

    def verify_table(self, name, config=''):
        self.client.verify_table(self.id, name, config)

    def begin_transaction(self, config=''):
        self.client.begin_transaction(self.id, config)

    def commit_transaction(self):
        self.client.begin_transaction(self.id)

    def rollback_transaction(self):
        self.client.rollback_transaction(self.id)

    def checkpoint(self, config=''):
        self.client.checkpoint(self.id, config)


class Connection:
    def __init__(self, uri, config=''):
        url = urlparse(uri)
        parts = url[1].split(':')
        host = parts[0]
        if len(parts) > 1:
            port = int(parts[1])
        else:
            port = 9090
        home = url[2]

        socket = TSocket.TSocket(host, port)
        self.transport = TTransport.TBufferedTransport(socket)
        protocol = TBinaryProtocol.TBinaryProtocol(self.transport)
        self.client = WiredTiger.Client(protocol)
        self.transport.open()

        self.id = self.client.open(home, config)

    def close(self, config=''):
        self.client.close_connection(self.id, config)
        self.transport.close()

    def version(self):
        v = self.client.version(self.id, config)
        return v.version_string, v.major, v.minor, v.patch

    def open_session(self, config=''):
        id = self.client.open_session(self.id, config)
        return Session(self, id)
