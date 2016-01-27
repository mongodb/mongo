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

import os
import wiredtiger, wttest, run
from wtscenario import check_scenarios, multiply_scenarios, number_scenarios

# test_join04.py
#    Join operations
# Joins with a custom extractor, using equality joins
class test_join04(wttest.WiredTigerTestCase):
    table_name1 = 'test_join04'
    nentries = 100

    # Return the wiredtiger_open extension argument for a shared library.
    def extensionArg(self, exts):
        extfiles = []
        for ext in exts:
            (dirname, name, libname) = ext
            if name != None and name != 'none':
                testdir = os.path.dirname(__file__)
                extdir = os.path.join(run.wt_builddir, 'ext', dirname)
                extfile = os.path.join(
                    extdir, name, '.libs', 'libwiredtiger_' + libname + '.so')
                if not os.path.exists(extfile):
                    self.skipTest('extension "' + extfile + '" not built')
                if not extfile in extfiles:
                    extfiles.append(extfile)
        if len(extfiles) == 0:
            return ''
        else:
            return ',extensions=["' + '","'.join(extfiles) + '"]'

    # Override WiredTigerTestCase, we have extensions.
    def setUpConnectionOpen(self, dir):
        extarg = self.extensionArg([('extractors', 'csv', 'csv_extractor')])
        connarg = 'create,error_prefix="{0}: ",{1}'.format(
            self.shortid(), extarg)
        conn = self.wiredtiger_open(dir, connarg)
        self.pr(`conn`)
        return conn

    # JIRA WT-2308:
    # Test extractors with equality joins
    def test_join_extractor(self):
        self.session.create('table:join04',
                            'key_format=i,value_format=S,columns=(k,v)')
        self.session.create('index:join04:index1',
                       'key_format=i,extractor=csv,' +
                       'app_metadata={"format" : "i", "field" : "0"}')
        self.session.create('index:join04:index2',
                       'key_format=i,extractor=csv,' +
                       'app_metadata={"format" : "i", "field" : "1"}')

        cursor1 = self.session.open_cursor('table:join04', None, None)
        cursor1[1] = '10,21'
        cursor1[2] = '10,22'
        cursor1.close()

        cursor1 = self.session.open_cursor('index:join04:index1', None, None)
        cursor1.set_key(10)
        cursor1.search()
        cursor2 = self.session.open_cursor('index:join04:index2', None, None)
        cursor2.set_key(22)
        cursor2.search()

        jcursor = self.session.open_cursor('join:table:join04', None, None)
        self.session.join(jcursor, cursor1, 'compare=eq')
        self.session.join(jcursor, cursor2, 'compare=eq')

        found = 0
        while jcursor.next() == 0:
            [k] = jcursor.get_keys()
            [v] = jcursor.get_values()
            self.assertEqual(k, 2)
            self.assertEqual(v, '10,22')
            found += 1
        self.assertEqual(found, 1)
        jcursor.close()
        cursor1.close()
        cursor2.close()

    # More tests using extractors with equality joins
    def test_join_extractor_more(self):
        self.session.create('table:join04',
                            'key_format=i,value_format=S,columns=(k,v)')
        self.session.create('index:join04:index1',
                       'key_format=i,extractor=csv,' +
                       'app_metadata={"format" : "i", "field" : "0"}')
        self.session.create('index:join04:index2',
                       'key_format=i,extractor=csv,' +
                       'app_metadata={"format" : "i", "field" : "1"}')
        self.session.create('index:join04:index3',
                       'key_format=i,extractor=csv,' +
                       'app_metadata={"format" : "i", "field" : "2"}')

        jcursor = self.session.open_cursor('join:table:join04', None, None)
        cursor1 = self.session.open_cursor('table:join04', None, None)
        k = 1
        for v in ['10,21,30','10,22,30','10,23,30',
                  '11,21,30','11,22,30','11,23,30',
                  '10,21,31','10,22,31','10,23,31',
                  '10,21,30','11,22,31','12,23,32']:
            cursor1[k] = v
            k += 1
        cursor1.close()

        # A list of tests, one per line, each entry is:
        #    [[list of inputs], [list of outputs]]
        tests = [
            [[10,22,30], ['10,22,30']],
            [[10,21,30], ['10,21,30','10,21,30']],
            [[11], ['11,21,30','11,22,30','11,23,30','11,22,31']],
            [[None,22], ['10,22,30','11,22,30','10,22,31','11,22,31']]]

        for t in tests:
            jcursor = self.session.open_cursor('join:table:join04', None, None)
            ins = t[0]
            outs = t[1]
            cursors = []
            n = 0
            for k in ins:
                n += 1
                if k == None: continue
                uri = 'index:join04:index' + str(n)
                c = self.session.open_cursor(uri, None, None)
                c.set_key(k)
                self.assertEqual(c.search(), 0)
                cursors.append(c)
                self.session.join(jcursor, c, 'compare=eq')
            while jcursor.next() == 0:
                [k] = jcursor.get_keys()
                [v] = jcursor.get_values()
                #self.tty('got=' + str(v) + ' at key=' + str(k))
                self.assertTrue(v in outs)
                outs.remove(v)
            self.assertEqual(len(outs), 0)
            jcursor.close()
            for c in cursors:
                c.close()


if __name__ == '__main__':
    wttest.run()
