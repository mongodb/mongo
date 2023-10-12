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
#

from test_verbose01 import test_verbose_base
from wtscenario import make_scenarios
import wiredtiger, wttest

# test_verbose04.py
# Verify the use of the `all` field to set verbose categories.

# Enabling tiered alters the logs produced by WiredTiger during this test, 
# breaking our assumptions about what log output to expect. This doesn't 
# impact the logic under test (the "all" configuration field) so we'll 
# disable this test under tiered."
@wttest.skip_for_hook("tiered", "Enabling tiered alters the logs produced by WiredTiger")
class test_verbose04(test_verbose_base):

    format = [
        ('flat', dict(is_json=False)),
        ('json', dict(is_json=True)),
    ]
    scenarios = make_scenarios(format)

    collection_cfg = 'key_format=S,value_format=S'
    
    # Define all the verbose flags.
    all_verbose_categories = [
      'WT_VERB_API',
      'WT_VERB_BACKUP',
      'WT_VERB_BLOCK',
      'WT_VERB_BLKCACHE', 
      'WT_VERB_CHECKPOINT',
      'WT_VERB_CHECKPOINT_CLEANUP',
      'WT_VERB_CHECKPOINT_PROGRESS',
      'WT_VERB_CHUNKCACHE',
      'WT_VERB_COMPACT',
      'WT_VERB_COMPACT_PROGRESS',
      'WT_VERB_ERROR_RETURNS',
      'WT_VERB_EVICT',
      'WT_VERB_EVICT_STUCK',
      'WT_VERB_EVICTSERVER',
      'WT_VERB_FILEOPS',
      'WT_VERB_GENERATION',
      'WT_VERB_HANDLEOPS',
      'WT_VERB_LOG',
      'WT_VERB_HS',
      'WT_VERB_HS_ACTIVITY',
      'WT_VERB_LSM',
      'WT_VERB_LSM_MANAGER',
      'WT_VERB_METADATA',
      'WT_VERB_MUTEX',
      'WT_VERB_PREFETCH',
      'WT_VERB_OUT_OF_ORDER',
      'WT_VERB_OVERFLOW',
      'WT_VERB_READ',
      'WT_VERB_RECONCILE',
      'WT_VERB_RECOVERY',
      'WT_VERB_RECOVERY_PROGRESS',
      'WT_VERB_RTS',
      'WT_VERB_SALVAGE',
      'WT_VERB_SHARED_CACHE',
      'WT_VERB_SPLIT',
      'WT_VERB_TEMPORARY',
      'WT_VERB_THREAD_GROUP',
      'WT_VERB_TIMESTAMP',
      'WT_VERB_TIERED',
      'WT_VERB_TRANSACTION',
      'WT_VERB_VERIFY',
      'WT_VERB_VERSION',
      'WT_VERB_WRITE'
    ]

    # Enable all categories at once.
    def test_verbose_all(self):
        # Close the initial connection. We will be opening new connections with different verbosity
        # settings throughout this test.
        self.close_conn()
  
        # Test passing a single verbose category, 'all' along with the verbosity level
        # WT_VERBOSE_DEBUG_1 (1). Ensuring the verbose output generated matches any of the existing verbose categories.
        # 'all' category.
        with self.expect_verbose(['all:1'], self.all_verbose_categories, self.is_json) as conn:
            # Perform a set of simple operations to generate verbose messages from different categories.
            uri = 'table:test_verbose04_all'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            c = session.open_cursor(uri)
            c['all'] = 'all'
            c.close()
            session.create(uri, self.collection_cfg)
            session.compact(uri)
            session.close()
            
        # At this time, no verbose messages should be generated with the following set of operations and the verbosity level 
        # WT_VERBOSE_INFO (0), hence we don't expect any output.
        with self.expect_verbose(['all:0'], self.all_verbose_categories, self.is_json, False) as conn:
            uri = 'table:test_verbose04_all'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            c = session.open_cursor(uri)
            c['all'] = 'all'
            c.close()
            session.close()
            
        # Test passing another single verbose category, 'all' with different verbosity levels.
        # Since there are verbose messages with the category WT_VERB_COMPACT and the verbosity
        # levels WT_VERBOSE_INFO (0) through WT_VERBOSE_DEBUG_5 (5), we can test them all.
        cfgs = ['all:0', 'all:1', 'all:2', 'all:3', 'all:4', 'all:5']
        for cfg in cfgs:
            with self.expect_verbose([cfg], self.all_verbose_categories, self.is_json) as conn:
                # Create a simple table to invoke compaction on. We aren't doing anything
                # interesting with the table, we want to simply invoke a compaction pass to generate
                # verbose messages.
                uri = 'table:test_verbose04_all'
                session = conn.open_session()
                session.create(uri, self.collection_cfg)
                session.compact(uri)
                session.close()
                
    # Test use cases passing multiple verbose categories, ensuring we only produce verbose output
    # for specified categories.
    def test_verbose_multiple(self):
        self.close_conn()
        # Test passing multiple verbose categories, being 'api' & 'all' & 'version' with different dedicated
        # verbosity levels to each category. Ensuring the only verbose output generated is related
        # to those two categories.
        cfgs = ['api:0,all:1,version:0', 'version:0,all,api:0']
        
        #all_verbose_categories_except_api_and_version contains all verbose flags except WT_VERB_API and WT_VERB_VERSION.
        all_verbose_categories_except_api_and_version = self.all_verbose_categories.copy()
        all_verbose_categories_except_api_and_version.remove('WT_VERB_API')
        all_verbose_categories_except_api_and_version.remove('WT_VERB_VERSION')
            
        for cfg in cfgs:
            with self.expect_verbose([cfg], all_verbose_categories_except_api_and_version, self.is_json) as conn:
                # Perform a set of simple API operations (table creations and cursor operations) to
                # generate verbose API messages. Beyond opening the connection resource, we
                # shouldn't need to do anything special for the version category.
                uri = 'table:test_verbose04_all'
                session = conn.open_session()
                session.create(uri, self.collection_cfg)
                c = session.open_cursor(uri)
                c['multiple'] = 'multiple'
                c.close()

if __name__ == '__main__':
    wttest.run()
