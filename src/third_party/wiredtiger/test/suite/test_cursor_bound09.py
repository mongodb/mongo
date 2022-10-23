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

import wiredtiger, wttest
from wiredtiger import wiredtiger_strerror, WT_PREPARE_CONFLICT, WiredTigerError
from wtscenario import make_scenarios
from wtbound import bound_base

# test_cursor_bound09.py
#    Test that cursor API usage generates expected error in prepared state with bounded cursors.
class test_cursor_bound09(bound_base):
    file_name = 'test_cursor_bound09'
    value_format= 'S'

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False)),
        ('colgroup', dict(uri='table:', use_colgroup=True))
    ]

    key_format_values = [
        ('string', dict(key_format='S')),
        ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('bytes', dict(key_format='u')),
        ('composite_string', dict(key_format='SSS')),
        ('composite_int_string', dict(key_format='iS')),
        ('composite_complex', dict(key_format='iSru')),
    ]

    config = [
        ('inclusive-evict', dict(lower_inclusive=True,upper_inclusive=True,evict=True)),
        ('no-inclusive-evict', dict(lower_inclusive=False,upper_inclusive=False,evict=True)),
        ('lower-inclusive-evict', dict(lower_inclusive=True,upper_inclusive=False,evict=True)),
        ('upper-inclusive-evict', dict(lower_inclusive=False,upper_inclusive=True,evict=True)),
        ('inclusive-no-evict', dict(lower_inclusive=True,upper_inclusive=True,evict=False)),
        ('lower-inclusive-no-evict', dict(lower_inclusive=True,upper_inclusive=False,evict=False)),
        ('no-inclusive-no-evict', dict(lower_inclusive=False,upper_inclusive=False,evict=False))        
    ]

    ignore_prepare = [
        ('ignore_prepare',dict(ignore_prepare=True)),
        ('no_ignore_prepare',dict(ignore_prepare=False))
    ]

    scenarios = make_scenarios(types, key_format_values, config, ignore_prepare)
    
    # Test ignore prepare.
    def test_cursor_bound_prepared(self):

        # Test bound api: Prepare conflict on search, search_near and next with key set in middle of bounds.
        cursor = self.create_session_and_cursor()
        session2 = self.conn.open_session()
        
        # Prepare keys 30-35
        self.session.begin_transaction()
        for i in range (30,36):
            cursor[self.gen_key(i)] = "updated value" + str(i)
        self.session.prepare_transaction('prepare_timestamp=2')

        if (self.ignore_prepare):
            session2.begin_transaction('ignore_prepare=true')
        else:
            session2.begin_transaction()

        cursor2 = session2.open_cursor(self.uri + self.file_name)
        cursor_ops = [cursor2.search, cursor2.search_near, cursor2.next]

        for op in cursor_ops:
            cursor2.reset()
            self.set_bounds(cursor2, 20, "lower", self.lower_inclusive)
            self.set_bounds(cursor2, 40, "upper", self.upper_inclusive)
            cursor2.set_key(self.gen_key(30))

            try:
                if(self.ignore_prepare):
                    self.assertEqual(op(),0)
                else:
                    op()
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    pass
                else:
                    raise e
        session2.rollback_transaction()    

        # Test bound api: Prepare conflict on search, search_near and next with key set on the bounds.
        if (self.ignore_prepare):
            session2.begin_transaction('ignore_prepare=true')
        else:
            session2.begin_transaction()
        cursor_ops = [cursor2.search_near, cursor2.search, cursor2.next]
        
        for op in cursor_ops:
            cursor2.reset()
            self.set_bounds(cursor2, 30, "lower", self.lower_inclusive)
            self.set_bounds(cursor2, 40, "upper", self.upper_inclusive)
            cursor2.set_key(self.gen_key(30))
            try:
                if (self.ignore_prepare):
                    # Search on the non-inclusive lower bound is not expected to suceed.
                    if (not self.lower_inclusive and op == cursor2.search):
                        self.assertEqual(op(), wiredtiger.WT_NOTFOUND)
                    else:
                        self.assertGreaterEqual(op(),0)
                else:
                    op()
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    pass
                else:
                    raise e
        session2.rollback_transaction()

        # Test bound api: Prepare conflict with prev. Validate keys that aren't in prepared range.
        if (self.ignore_prepare):
            session2.begin_transaction('ignore_prepare=true')
        else:
            session2.begin_transaction()

        cursor2.reset()
        self.set_bounds(cursor2, 30, "lower", self.lower_inclusive)
        self.set_bounds(cursor2, 40, "upper", self.upper_inclusive)

        while True:
            try:
                if (self.ignore_prepare):
                    ret = cursor2.prev()
                    if(ret == wiredtiger.WT_NOTFOUND): 
                        break
                else:
                    cursor2.prev()
                    self.assertGreaterEqual(cursor2.get_key(),self.check_key(35))
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    break
                else:
                    raise (e)
        session2.rollback_transaction()

        # Test bound api:: Prepare conflict with search_near out of the bounds.
        self.session.rollback_transaction()

        # Prepare keys 29-30
        self.session.begin_transaction()
        for i in range (29,31):
            cursor[self.gen_key(i)] = "updated value" + str(i)
        self.session.prepare_transaction('prepare_timestamp=2')

        if (self.ignore_prepare):
            session2.begin_transaction('ignore_prepare=true')
        else:
            session2.begin_transaction()
        cursor2.reset()
        self.set_bounds(cursor2, 30, "lower", self.lower_inclusive)
        self.set_bounds(cursor2, 40, "upper", self.upper_inclusive)
        cursor2.set_key(self.gen_key(20))

        try:
            if (not(self.ignore_prepare)):
                self.assertGreaterEqual(cursor2.search_near(), 0)
                self.assertEqual(cursor2.get_key(), self.check_key(31))
        except WiredTigerError as e:
            if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                self.assertEqual(cursor2.get_key(),self.check_key(20))
                pass
            else:
                raise e
        session2.rollback_transaction() 

        # Test bound api:Prepare conflict with next with non-inclusive bounds.
        if (self.ignore_prepare):
            session2.begin_transaction('ignore_prepare=true')
        else:
            session2.begin_transaction()
        cursor2.reset()
        self.set_bounds(cursor2, 30, "lower", inclusive = False)
        self.set_bounds(cursor2, 40, "upper", inclusive = False)
        
        try:
            cursor2.next()
        except WiredTigerError as e:
            if wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                self.assertEqual(cursor2.get_key(),self.check_key(30))
                pass
            else:
                raise e
        session2.rollback_transaction() 

if __name__ == '__main__':
    wttest.run()
