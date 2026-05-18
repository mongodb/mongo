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

import os, wiredtiger

# These routines help run the various storage sources. They are required to manage
# generation of storage source specific configurations.

# Generate a storage store specific authentication token.
def get_auth_token(storage_source):
    auth_token = None
    if storage_source == 'dir_store':
        # Fake a secret token.
        auth_token = "Secret"
    return auth_token

# Buckets configured for the storage source.
buckets = {
    "dir_store": ['bucket1', 'bucket2'],
}

# Get name of the bucket at specified index in the list.
def get_bucket_name(storage_source, i):
    return buckets[storage_source][i]

# Set up configuration
def get_conn_config(storage_source):
    if not storage_source.is_tiered_scenario():
            return ''
    if storage_source.ss_name == 'dir_store' and not os.path.exists(storage_source.bucket):
            os.mkdir(storage_source.bucket)
    return \
        'statistics=(all),' + \
        'tiered_storage=(auth_token=%s,' % storage_source.auth_token + \
        'bucket=%s,' % storage_source.bucket + \
        'bucket_prefix=%s,' % storage_source.bucket_prefix + \
        'name=%s,' % storage_source.ss_name

# Set up configuration
def get_shared_conn_config(storage_source):
    if not storage_source.is_tiered_shared_scenario():
            return ''
    if storage_source.ss_name == 'dir_store' and not os.path.exists(storage_source.bucket):
            os.mkdir(storage_source.bucket)
    return \
        'statistics=(all),' + \
        'tiered_storage=(auth_token=%s,' % storage_source.auth_token + \
        'bucket=%s,' % storage_source.bucket + \
        'bucket_prefix=%s,' % storage_source.bucket_prefix + \
        'name=%s,' % storage_source.ss_name + \
        'shared=true,'

def get_check(storage_source, tc, base, n):
    for i in range(base, n):
        storage_source.assertEqual(tc[str(i)], str(i))
    tc.set_key(str(n))
    storage_source.assertEqual(tc.search(), wiredtiger.WT_NOTFOUND)

def gen_tiered_storage_sources(random_prefix='', test_name='', tiered_only=False, tiered_shared=False):
    tiered_storage_sources = [
        ('dir_store', dict(is_tiered = True,
            is_tiered_shared = tiered_shared,
            is_local_storage = True,
            auth_token = get_auth_token('dir_store'),
            bucket = get_bucket_name('dir_store', 0),
            bucket1 = get_bucket_name('dir_store', 1),
            bucket_prefix = "pfx_",
            bucket_prefix1 = "pfx1_",
            bucket_prefix2 = 'pfx2_',
            num_ops=100,
            ss_name = 'dir_store')),
        # This must be the last item as we separate the non-tiered from the tiered items later on.
        ('non_tiered', dict(is_tiered = False)),
    ]

    # Return a sublist to use for the tiered test scenarios as last item on list is not a scenario
    # for the tiered tests.
    if tiered_only:
        return tiered_storage_sources[:-1]

    return tiered_storage_sources

# This mixin class provides tiered storage configuration methods.
class TieredConfigMixin:
    # Returns True if the current scenario is tiered.
    def is_tiered_scenario(self):
        return hasattr(self, 'is_tiered') and self.is_tiered

    # Returns True if the current scenario is tiered shared.
    def is_tiered_shared_scenario(self):
        return hasattr(self, 'is_tiered_shared') and self.is_tiered_shared

    # Setup connection config.
    def conn_config(self):
        if not self.is_tiered_shared_scenario():
            return self.tiered_conn_config()
        else:
            return self.tiered_shared_conn_config()

    # Can be overridden
    def additional_conn_config(self):
        return ''

    # Setup tiered connection config.
    def tiered_conn_config(self):
        # Handle non_tiered storage scenarios.
        if not self.is_tiered_scenario():
            return self.additional_conn_config()

        # Setup directories structure for local tiered storage.
        if self.is_local_storage:
            bucket_full_path = os.path.join(self.home, self.bucket)
            if not os.path.exists(bucket_full_path):
                os.mkdir(bucket_full_path)

        # Build tiered storage connection string.
        # Any additional configuration appears first to override this configuration.
        return \
            self.additional_conn_config() + \
            ',tiered_storage=(auth_token=%s,' % self.auth_token + \
            'bucket=%s,' % self.bucket + \
            'bucket_prefix=%s,' % self.bucket_prefix + \
            'name=%s),' % self.ss_name

    def tiered_shared_conn_config(self):
        # Handle non_tiered storage scenarios.
        if not self.is_tiered_shared_scenario():
            return self.additional_conn_config()

        # Setup directories structure for local tiered storage.
        if self.is_local_storage:
            bucket_full_path = os.path.join(self.home, self.bucket)
            if not os.path.exists(bucket_full_path):
                os.mkdir(bucket_full_path)

        # Build tiered storage connection string.
        # Any additional configuration appears first to override this configuration.
        return \
            self.additional_conn_config() + ',' + \
            ',tiered_storage=(auth_token=%s,' % self.auth_token + \
            'bucket=%s,' % self.bucket + \
            'bucket_prefix=%s,' % self.bucket_prefix + \
            'name=%s, shared=true),' % self.ss_name

    # Load the storage sources extension.
    def conn_extensions(self, extlist):
        return self.tiered_conn_extensions(extlist)

    # Returns configuration to be passed to the extension.
    # Call may override, in which case, they probably want to
    # look at self.is_local_storage or self.ss_name, as every
    # extension has their own configurations that are valid.
    #
    # Some possible values to return: 'verbose=1'
    # or for dir_store: 'verbose=1,delay_ms=13,force_delay=30'
    def tiered_extension_config(self):
        return ''

    # Load tiered storage source extension.
    def tiered_conn_extensions(self, extlist):
        # Handle non_tiered storage scenarios.
        if not self.is_tiered_scenario():
            return ''

        config = self.tiered_extension_config()
        if config == None:
            config = ''
        elif config != '':
            config = '=(config=\"(%s)\")' % config

        if not self.is_local_storage:
            extlist.skip_if_missing = True
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.ss_name + config)

    def download_objects(self, bucket_name, prefix):
        pass

