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

import wttest
from wtscenario import make_scenarios
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources

# test_alter01.py
#    Smoke-test the session alter operations.
class test_alter01(TieredConfigMixin, wttest.WiredTigerTestCase):
    name = "alter01"
    entries = 100

    # Data source types
    types = [
        ('file', dict(uri='file:', use_cg=False, use_index=False)),
        ('lsm', dict(uri='lsm:', use_cg=False, use_index=False)),
        ('table-cg', dict(uri='table:', use_cg=True, use_index=False)),
        ('table-index', dict(uri='table:', use_cg=False, use_index=True)),
        ('table-simple', dict(uri='table:', use_cg=False, use_index=False)),
    ]

    # Settings for access_pattern_hint
    hints = [
        ('default', dict(acreate='')),
        ('none', dict(acreate='none')),
        ('random', dict(acreate='random')),
        ('sequential', dict(acreate='sequential')),
    ]
    access_alter=('', 'none', 'random', 'sequential')

    # Settings for cache_resident
    resid = [
        ('default', dict(ccreate='')),
        ('false', dict(ccreate='false')),
        ('true', dict(ccreate='true')),
    ]
    reopen = [
        ('no-reopen', dict(reopen=False)),
        ('reopen', dict(reopen=True)),
    ]
    cache_alter=('', 'false', 'true')

    # Build all scenarios
    tiered_storage_sources = gen_tiered_storage_sources()
    scenarios = make_scenarios(tiered_storage_sources, types, hints, resid, reopen)

    def verify_metadata(self, metastr):
        if metastr == '':
            return
        cursor = self.session.open_cursor('metadata:', None, None)
        #
        # Walk through all the metadata looking for the entries that are
        # the file URIs for components of the table.
        #
        found = False
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            key = cursor.get_key()
            check_meta = ((key.find("lsm:") != -1 or key.find("file:") != -1) \
                and key.find(self.name) != -1)
            if check_meta:
                value = cursor[key]
                found = True
                self.assertTrue(value.find(metastr) != -1)
        cursor.close()
        self.assertTrue(found == True)

    # Alter: Change the access pattern hint after creation
    def test_alter01_access(self):
        if self.is_tiered_scenario() and (self.uri == 'lsm:' or self.uri == 'file:'):
            self.skipTest('Tiered storage does not support LSM or file URIs.')

        uri = self.uri + self.name
        create_params = 'key_format=i,value_format=i,'
        complex_params = ''
        #
        # If we're not explicitly setting the parameter, then don't
        # modify create_params to test using the default.
        #
        if self.acreate != '':
            access_param = 'access_pattern_hint=%s' % self.acreate
            create_params += '%s,' % access_param
            complex_params += '%s,' % access_param
        else:
            # NOTE: This is hard-coding the default value.  If the default
            # changes then this will fail and need to be fixed.
            access_param = 'access_pattern_hint=none'
        if self.ccreate != '':
            cache_param = 'cache_resident=%s' % self.ccreate
            create_params += '%s,' % cache_param
            complex_params += '%s,' % cache_param
        else:
            # NOTE: This is hard-coding the default value.  If the default
            # changes then this will fail and need to be fixed.
            cache_param = 'cache_resident=false'

        cgparam = ''
        if self.use_cg or self.use_index:
            cgparam = 'columns=(k,v),'
        if self.use_cg:
            cgparam += 'colgroups=(g0),'

        self.session.create(uri, create_params + cgparam)
        # Add in column group or index settings.
        if self.use_cg:
            cgparam = 'columns=(v),'
            suburi = 'colgroup:' + self.name + ':g0'
            self.session.create(suburi, complex_params + cgparam)
        if self.use_index:
            suburi = 'index:' + self.name + ':i0'
            self.session.create(suburi, complex_params + cgparam)

        # Put some data in table.
        c = self.session.open_cursor(uri, None)
        for k in range(self.entries):
            c[k+1] = 1
        c.close()

        # Verify the string in the metadata
        self.verify_metadata(access_param)
        self.verify_metadata(cache_param)

        # Run through all combinations of the alter commands
        # for all allowed settings.  This tests having only one or
        # the other set as well as having both set.  It will also
        # cover trying to change the setting to its current value.
        for a in self.access_alter:
            alter_param = ''
            access_str = ''
            if a != '':
                access_str = 'access_pattern_hint=%s' % a
            for c in self.cache_alter:
                alter_param = '%s' % access_str
                cache_str = ''
                if c != '':
                    cache_str = 'cache_resident=%s' % c
                    alter_param += ',%s' % cache_str
                if alter_param != '':
                    self.session.alter(uri, alter_param)
                    if self.reopen:
                        self.reopen_conn()
                    special = self.use_cg or self.use_index
                    if not special:
                        self.verify_metadata(access_str)
                        self.verify_metadata(cache_str)
                    else:
                        self.session.alter(suburi, alter_param)
                        self.verify_metadata(access_str)
                        self.verify_metadata(cache_str)

if __name__ == '__main__':
    wttest.run()
