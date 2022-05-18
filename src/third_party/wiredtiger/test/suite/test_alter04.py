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
from helper_tiered import TieredConfigMixin, tiered_storage_sources
from wtscenario import make_scenarios

# test_alter04.py
#    Smoke-test the session alter operations.
#    This test confirms os_cache_dirty_max and os_cache_max.
class test_alter04(TieredConfigMixin, wttest.WiredTigerTestCase):
    name = "alter04"
    entries = 100
    cache_alter=('1M', '100K')
    # Settings for os_cache[_dirty]_max.
    types = [
        ('file', dict(uri='file:', use_cg=False, use_index=False)),
        ('lsm', dict(uri='lsm:', use_cg=False, use_index=False)),
        ('table-cg', dict(uri='table:', use_cg=True, use_index=False)),
        ('table-index', dict(uri='table:', use_cg=False, use_index=True)),
        ('table-simple', dict(uri='table:', use_cg=False, use_index=False)),
    ]
    sizes = [
        ('default', dict(ocreate='')),
        ('1M', dict(ocreate='1M')),
        ('200K', dict(ocreate='200K')),
    ]
    reopen = [
        ('no-reopen', dict(reopen=False)),
        ('reopen', dict(reopen=True)),
    ]
    settings = [
        ('cache', dict(setting='os_cache_max')),
        ('cache_dirty', dict(setting='os_cache_dirty_max')),
    ]
    scenarios = make_scenarios(tiered_storage_sources, types, sizes, reopen, settings)

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

    # Alter: Change the setting after creation
    def test_alter04_cache(self):
        if self.is_tiered_scenario() and (self.uri == 'lsm:' or self.uri == 'file:'):
            self.skipTest('Tiered storage does not support LSM or file URIs.')
        
        uri = self.uri + self.name
        create_params = 'key_format=i,value_format=i,'
        complex_params = ''
        #
        # If we're not explicitly setting the parameter, then don't
        # modify create_params to test using the default.
        #
        if self.ocreate != '':
            new_param = '%s=%s' % (self.setting, self.ocreate)
            create_params += '%s,' % new_param
            complex_params += '%s,' % new_param
        else:
            # NOTE: This is hard-coding the default value.  If the default
            # changes then this will fail and need to be fixed.
            new_param = '%s=0' % self.setting

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
        self.verify_metadata(new_param)

        # Run through all combinations of the alter commands
        # for all allowed settings.
        for a in self.cache_alter:
            alter_param = '%s=%s' % (self.setting, a)
            self.alter(uri, alter_param)
            if self.reopen:
                self.reopen_conn()
            special = self.use_cg or self.use_index
            if not special:
                self.verify_metadata(alter_param)
            else:
                self.alter(suburi, alter_param)
                self.verify_metadata(alter_param)

if __name__ == '__main__':
    wttest.run()
