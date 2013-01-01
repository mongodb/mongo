#
#
# Copyright (c) 2008-2013 WiredTiger, Inc.
#	All rights reserved.
#
# See the file LICENSE for redistribution information.
#
# WiredTiger API implementation

'''
WiredTiger Python API implementation.
'''

import struct
from urlparse import urlparse
from wiredtiger import pack, unpack
from wiredtiger.util import parse_config

# Import the BDB symbols with the 'db.' prefix, it avoids polluting the
# namespace of this package
from bsddb3 import db

class Table:
    def __init__(self, db, name, key_format='u', value_format='u', columns=(,), colgroups=(,), indices=(,)):
        self.db = db
        self.name = name
        self.key_format = key_format
        self.value_format = value_format
        self.columns = columns
        self.colgroups = colgroups
        self.indices = indices

    def close(self):
        self.db.close(db.DB_NOSYNC)

    def check_schema(self, key_format='u', value_format='u', columns=(,), colgroups=(,), indices=(,)):
        if (self.key_format != key_format or
          self.value_format != value_format or
          self.columns != columns or
          self.colgroups != colgroups or
          self.indices != indices):
            raise 'Schemas don\'t match for table "' + self.name + '"'

class Cursor:
    def __init__(self, session, table):
        self.session = session
        self.table = table
        self.key_format = table.key_format
        self.value_format = table.value_format
        self.dbc = table.db.cursor()

    def close(self, config=''):
        self.dbc.close()
        self.session.cursors.remove(self)

    def get_key(self):
        return unpack(self.key_format, self.key)

    def get_value(self):
        return unpack(self.key_format, self.value)

    def set_key(self, *args):
        self.key = pack(self.key_format, *args)

    def set_value(self, *args):
        self.value = pack(self.value_format, *args)

    def first(self):
        self.key, self.value = self.dbc.first()

    def last(self):
        self.key, self.value = self.dbc.last()

    def next(self):
        self.key, self.value = self.dbc.next()

    def prev(self):
        self.key, self.value = self.dbc.prev()

    def search(self):
        searchkey = self.key
        self.key, self.value = self.dbc.set_range(self.key)
        return (self.key == searchkey)

    def insert(self):
        self.dbc.put(self.key, self.value)
        return self.key

    def update(self):
        self.dbc.put(self.key, self.value, db.DB_CURRENT)

    def delete(self):
        self.dbc.delete()


class Session:
    def __init__(self, conn, id):
        self.conn = conn
        self.cursors = []
        self.tables = {'schema' : conn.schematab}

    def _close_cursors(self):
        # Work on a copy of the list because Cursor.close removes itself
        for c in self.cursors[:]:
            c.close()

    def close(self, config=''):
        self._close_cursors()
        self.conn.sessions.remove(self)

    def open_cursor(self, uri, config=''):
        c = self.conn._open_cursor(self, uri, config)
        self.cursors.append(c)
        return c

    def dup_cursor(self, c, config=''):
        dupc = c.dup()
        self.cursors.append(dupc)
        return dupc

    def _open_table(self, name):
        schema_cur = Cursor(self, self.conn.schematab)
        schema_cur.set_key(name)
        if schema_cur.search():
            k, v, c, cset, idx = schema_cur.get_value()
            c = tuple(parse_config(c))
            cset = tuple(parse_config(cset))
            idx = tuple(parse_config(idx))
            self.tables[name] = Table(k, v, c, cset, idx)

    def create_table(self, name, config=''):
        schema = {}
        for k, v in parse_config(config):
            if k in ('key_format', 'value_format', 'columns'):
                schema[k] = v
            elif k.startswith('colgroup'):
                schema['colgroup'] = schema.get('colgroup', (,)) + (k[len('colgroup')+1:], v)
            elif k.startswith('index'):
                schema['indices'] = schema.get('indices', (,)) + (k[len('index')+1:], v)
            else:
                raise 'Unknown configuration "' + k + '"'
        if name in self.tables:
            self.tables[name].check_schema(**schema)
        # XXX else try to open the table and retry

    def rename_table(self, oldname, newname, config=''):
        pass

    def drop_table(self, name, config=''):
        pass

    def truncate_table(self, name, start=None, end=None, config=''):
        pass

    def verify_table(self, name, config=''):
        pass

    def begin_transaction(self, config=''):
        if self.cursors:
            raise 'Transactions cannot be started with cursors open'

    def commit_transaction(self):
        self._close_cursors()
        pass

    def rollback_transaction(self):
        self._close_cursors()
        pass

    def checkpoint(self, config=''):
        pass


class Connection:
    def __init__(self, uri, config=''):
        url = urlparse(uri)
        parts = url[1].split(':')
        self.host = parts[0]
        if len(parts) > 1:
            port = int(parts[1])
        else:
            port = 9090

        self.home = url[2]
        self.sessions = []

        self.env = db.DBEnv();
        self.env.open(self.home,
          db.DB_PRIVATE | db.DB_CREATE | db.DB_INIT_MPOOL | db.DB_THREAD)
        schemadb = db.DB(self.env);
        schemadb.open("__wt_schema.db", None, db.DB_BTREE, db.DB_CREATE)

        # The schema of the schema table.
        self.schematab = Table(schemadb, key_format='S', value_format='SSSSS',
            columns=('name', 'key_format', 'value_format', 'colgroups', 'indices'))

    def close(self, config=''):
        # Work on a copy of the list because Session.close removes itself
        for s in self.sessions[:]:
            s.close()
        self.schematab.close()
        self.env.close()

    def version(self):
        return ("WiredTiger Python API 0.0.1", 0, 0, 1)

    def open_session(self, config=''):
        s = Session(self, config)
        self.sessions.append(s)
        return s

    def _open_cursor(self, session, uri, config):
        if uri == 'table:':
            return Cursor(session, self.schematab)
        elif uri.startswith('table:'):
            # XXX projections
            return Cursor(session, session._get_table(uri[6:]))
        # XXX application-specific cursor types?
        raise 'Unknown cursor type for "' + uri + '"'
