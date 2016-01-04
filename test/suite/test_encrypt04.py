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
# test_encrypt04.py
#   Test mismatches error conditions with encryption.
#

import os, run, random
import wiredtiger, wttest
from wtscenario import multiply_scenarios, number_scenarios
from suite_subprocess import suite_subprocess

# Test basic encryption with mismatched configuration
class test_encrypt04(wttest.WiredTigerTestCase, suite_subprocess):

    uri='table:test_encrypt04'

    # For tests that are mismatching, we use a secretkey. The 'rotn'
    # encryptor without a secretkey is too simple, and may leave
    # substantional portions of its input unchanged - a root page decoded
    # with simply the wrong keyid may appear valid when initially verified,
    # but may result in error on first use. The odds that a real encryptor
    # would leave a lot of its input unchanged is infinitesimally small.
    #
    # When both self.forceerror1 and self.forceerror2 occur, we set a config
    # flag when loading the rotn encryptor, which forces a particular error
    # return in rotn.decrypt. We look for that return back from
    # wiredtiger_open.
    encrypt_scen_1 = [
        ('none', dict( name1='none', keyid1='', secretkey1='')),
        ('rotn17abc', dict( name1='rotn', keyid1='17',
                                      secretkey1='ABC', forceerror1=True)),
        ('rotn11abc', dict( name1='rotn', keyid1='11', secretkey1='ABC')),
        ('rotn11xyz', dict( name1='rotn', keyid1='11', secretkey1='XYZ')),
        ('rotn11xyz_and_clear', dict( name1='rotn', keyid1='11',
                                      secretkey1='XYZ', fileinclear1=True))
    ]
    encrypt_scen_2 = [
        ('none', dict( name2='none', keyid2='', secretkey2='')),
        ('rotn17abc', dict( name2='rotn', keyid2='17', secretkey2='ABC')),
        ('rotn11abc', dict( name2='rotn', keyid2='11', secretkey2='ABC')),
        ('rotn11xyz', dict( name2='rotn', keyid2='11',
                                      secretkey2='XYZ', forceerror2=True)),
        ('rotn11xyz_and_clear', dict( name2='rotn', keyid2='11',
                                      secretkey2='XYZ', fileinclear2=True))
    ]
    scenarios = number_scenarios(multiply_scenarios \
                                 ('.', encrypt_scen_1, encrypt_scen_2))
    nrecords = 5000
    bigvalue = "abcdefghij" * 1001    # len(bigvalue) = 10010

    def __init__(self, *args, **kwargs):
        wttest.WiredTigerTestCase.__init__(self, *args, **kwargs)
        self.part = 1

    # Override WiredTigerTestCase, we have extensions.
    def setUpConnectionOpen(self, dir):
        forceerror = None
        if self.part == 1:
            self.name = self.name1
            self.keyid = self.keyid1
            self.secretkey = self.secretkey1
            self.fileinclear = self.fileinclear1 if \
                               hasattr(self, 'fileinclear1') else False
        else:
            self.name = self.name2
            self.keyid = self.keyid2
            self.secretkey = self.secretkey2
            self.fileinclear = self.fileinclear2 if \
                               hasattr(self, 'fileinclear2') else False
            if hasattr(self, 'forceerror1') and hasattr(self, 'forceerror2'):
                forceerror = "rotn_force_error=true"
        self.expect_forceerror = forceerror != None
        self.got_forceerror = False

        encarg = 'encryption=(name={0},keyid={1},secretkey={2}),'.format(
            self.name, self.keyid, self.secretkey)
        # If forceerror is set for this test, add a config arg to
        # the extension string. That signals rotn to return a (-1000)
        # error code, which we'll detect here.
        extarg = self.extensionArg([('encryptors', self.name, forceerror)])
        self.pr('encarg = ' + encarg + ' extarg = ' + extarg)
        completed = False
        try:
            conn = self.wiredtiger_open(dir,
                'create,error_prefix="{0}: ",{1}{2}'.format(
                 self.shortid(), encarg, extarg))
        except (BaseException) as err:
            # Capture the recognizable error created by rotn
            if str(-1000) in str(err):
                self.got_forceerror = True
            raise
        self.pr(`conn`)
        return conn

    def create_records(self, cursor, r, low, high):
        for idx in xrange(low, high):
            start = r.randint(0,9)
            key = self.bigvalue[start:r.randint(0,100)] + str(idx)
            val = self.bigvalue[start:r.randint(0,10000)] + str(idx)
            cursor.set_key(key)
            cursor.set_value(val)
            cursor.insert()

    def check_records(self, cursor, r, low, high):
        for idx in xrange(low, high):
            start = r.randint(0,9)
            key = self.bigvalue[start:r.randint(0,100)] + str(idx)
            val = self.bigvalue[start:r.randint(0,10000)] + str(idx)
            cursor.set_key(key)
            self.assertEqual(cursor.search(), 0)
            self.assertEquals(cursor.get_value(), val)

    # Return the wiredtiger_open extension argument for a shared library.
    def extensionArg(self, exts):
        extfiles = []
        for ext in exts:
            (dirname, name, extarg) = ext
            if name != None and name != 'none':
                testdir = os.path.dirname(__file__)
                extdir = os.path.join(run.wt_builddir, 'ext', dirname)
                extfile = os.path.join(
                    extdir, name, '.libs', 'libwiredtiger_' + name + '.so')
                if not os.path.exists(extfile):
                    self.skipTest('extension "' + extfile + '" not built')
                extfile = '"' + extfile + '"'
                if not extfile in extfiles:
                    s = extfile
                    if extarg != None:
                        s += "=(config=\"" + extarg + "\")"
                    extfiles.append(s)
        if len(extfiles) == 0:
            return ''
        else:
            return ',extensions=[' + ','.join(extfiles) + ']'

    # Evaluate expression, which either must succeed (if expect_okay)
    # or must fail (if !expect_okay).
    def check_okay(self, expect_okay, expr):
        completed = False
        if expect_okay:
            expr()
        else:
            # expect an error, and maybe error messages,
            # so turn off stderr checking.
            with self.expectedStderrPattern(''):
                try:
                    expr()
                    completed = True
                except:
                    pass
                self.assertEqual(False, completed)
        return expect_okay

    # Create a table with encryption values that are in error.
    def test_encrypt(self):
        params = 'key_format=S,value_format=S'
        if self.name == 'none' or self.fileinclear:
            params += ',encryption=(name=none)'
        else:
            params += ',encryption=(name=' + self.name + \
                      ',keyid=' + self.keyid + ')'

        self.session.create(self.uri, params)
        cursor = self.session.open_cursor(self.uri, None)
        r = random.Random()
        r.seed(0)
        self.create_records(cursor, r, 0, self.nrecords)
        cursor.close()

        # Now intentially expose the test to mismatched configuration
        self.part = 2
        self.name = self.name2
        self.keyid = self.keyid2
        self.secretkey = self.secretkey2

        is_same = (self.name1 == self.name2 and
                   self.keyid1 == self.keyid2 and
                   self.secretkey1 == self.secretkey2)

        # We expect an error if we specified different
        # encryption from one open to the next.
        expect_okay = is_same

        # Force the cache to disk, so we read
        # compressed/encrypted pages from disk.
        if self.check_okay(expect_okay, lambda: self.reopen_conn()):
            cursor = self.session.open_cursor(self.uri, None)
            r.seed(0)
            self.check_records(cursor, r, 0, self.nrecords)

            if not is_same:
                # With a configuration that has changed, we do a further test.
                # Add some more items with the current configuration.
                self.create_records(cursor, r, self.nrecords, self.nrecords * 2)
                cursor.close()

                # Force the cache to disk, so we read
                # compressed/encrypted pages from disk.
                # Now read both sets of data.
                self.reopen_conn()
                cursor = self.session.open_cursor(self.uri, None)
                r.seed(0)
                self.check_records(cursor, r, 0, self.nrecords)
                self.check_records(cursor, r, self.nrecords, self.nrecords * 2)
            cursor.close()
        self.assertEqual(self.expect_forceerror, self.got_forceerror)


if __name__ == '__main__':
    wttest.run()
