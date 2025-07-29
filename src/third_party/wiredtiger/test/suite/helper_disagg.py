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

import functools, os, wttest

# These routines help run the various page log sources used by disaggregated storage.
# They are required to manage the generation of disaggregated storage specific configurations.

# Set up configuration
def get_conn_config(disagg_storage):
    if not disagg_storage.is_disagg_scenario():
            return ''
    if disagg_storage.ds_name == 'palm' and not os.path.exists(disagg_storage.bucket):
            os.mkdir(disagg_storage.bucket)
    return \
        f'statistics=(all),name={disagg_storage.ds_name},lose_all_my_data=true'

def gen_disagg_storages(test_name='', disagg_only = False):
    disagg_storages = [
        ('palm', dict(is_disagg = True,
            is_local_storage = True,
            num_ops=100,
            ds_name = 'palm')),
        # This must be the last item as we separate the non-disagg from the disagg items later on.
        ('non_disagg', dict(is_disagg = False)),
    ]

    if disagg_only:
        return disagg_storages[:-1]

    return disagg_storages

# For disaggregated test cases, we generally want to ignore verbose warnings about RTS at shutdown.
def disagg_ignore_expected_output(testcase):
    testcase.ignoreStdoutPattern('WT_VERB_RTS')

# A decorator for a disaggregated test class, that ignores verbose warnings about RTS at shutdown.
# The class decorator takes a class as input, and returns a class to take its place.
def disagg_test_class(cls):
    class disagg_test_case_class(cls):
        @functools.wraps(cls, updated=())
        def __init__(self, *args, **kwargs):
            super(disagg_test_case_class, self).__init__(*args, **kwargs)
            disagg_ignore_expected_output(self)

        # Create an early_setup function, only if it hasn't already been overridden.
        if cls.early_setup == wttest.WiredTigerTestCase.early_setup:
            def early_setup(self):
                os.mkdir('follower')
                # Create the home directory for the PALM k/v store, and share it with the follower.
                os.mkdir('kv_home')
                os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

        # Load the page log extension, only if extensions hasn't already been specified.
        if cls.conn_extensions == wttest.WiredTigerTestCase.conn_extensions:
            def conn_extensions(self, extlist):
                if os.name == 'nt':
                    extlist.skip_if_missing = True
                return DisaggConfigMixin.conn_extensions(self, extlist)

    # Preserve the original name of the wrapped class, so that the test ID is unmodified.
    disagg_test_case_class.__name__ = cls.__name__
    disagg_test_case_class.__qualname__ = cls.__qualname__
    # Preserve the original module, as it is an integral part of the test's identity.
    disagg_test_case_class.__module__ = cls.__module__
    return disagg_test_case_class

# This mixin class provides disaggregated storage configuration methods.
class DisaggConfigMixin:
    palm_debug = False        # can be overridden in test class
    palm_config = None        # a string, can be overridden in test class
    palm_cache_size_mb = -1   # this uses the default, can be overridden

    # Returns True if the current scenario is disaggregated.
    def is_disagg_scenario(self):
        return hasattr(self, 'is_disagg') and self.is_disagg

    # Setup connection config.
    def conn_config(self):
        return self.disagg_conn_config()

    # Can be overridden
    def additional_conn_config(self):
        return ''

    # Setup disaggregated connection config.
    def disagg_conn_config(self):
        # Handle non_disaggregated storage scenarios.
        if not self.is_disagg_scenario():
            return self.additional_conn_config()

        # Setup directories structure for local disaggregated storage.
        if self.is_local_storage:
            bucket_full_path = os.path.join(self.home, self.bucket)
            if not os.path.exists(bucket_full_path):
                os.mkdir(bucket_full_path)

        # Build disaggregated storage connection string.
        # Any additional configuration appears first to override this configuration.
        return \
            self.additional_conn_config() + f'name={self.ds_name}),'

    # Load the storage sources extension.
    def conn_extensions(self, extlist):
        return self.disagg_conn_extensions(extlist)

    # Returns configuration to be passed to the extension.
    # Call may override, in which case, they probably want to
    # look at self.is_local_storage or self.ds_name, as every
    # extension has their own configurations that are valid.
    #
    # Some possible values to return: 'verbose=1'
    # or for palm: 'verbose=1,delay_ms=13,force_delay=30'
    # or 'materialization_delay_ms=1000'
    def disaggregated_extension_config(self):
        extension_config = ''
        if self.ds_name == 'palm':
            if self.palm_debug:
                extension_config += ',verbose=1'
            else:
                extension_config += ',verbose=0'
            if self.palm_cache_size_mb != -1:
                extension_config += f',cache_size_mb={self.palm_cache_size_mb}'
            if self.palm_config:
                extension_config += ',' + self.palm_config
        return extension_config

    # Load disaggregated storage extension.
    def disagg_conn_extensions(self, extlist):
        # Handle non_disaggregated storage scenarios.
        if not self.is_disagg_scenario():
            return ''

        config = self.disaggregated_extension_config()
        if config == None:
            config = ''
        elif config != '':
            config = f'=(config=\"({config})\")'

        # S3 store is built as an optional loadable extension, not all test environments build S3.
        if not self.is_local_storage:
            extlist.skip_if_missing = True
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', self.ds_name + config)

    # Get the information about the last completed checkpoint: ID, LSN, and metadata
    def disagg_get_complete_checkpoint_ext(self, conn=None):
        if conn is None:
            conn = self.conn
        page_log = conn.get_page_log('palm')

        session = conn.open_session('')
        r = page_log.pl_get_complete_checkpoint_ext(session)
        session.close()
        return r

    # Get the metadata about the last completed checkpoint
    def disagg_get_complete_checkpoint_meta(self, conn=None):
        (_, _, _, m) = self.disagg_get_complete_checkpoint_ext(conn)
        return m

    # Let the follower pick up the latest checkpoint
    def disagg_advance_checkpoint(self, conn_follower, conn_leader=None):
        m = self.disagg_get_complete_checkpoint_meta(conn_leader)
        conn_follower.reconfigure(f'disaggregated=(checkpoint_meta="{m}")')

    # Switch the leader and the follower
    def disagg_switch_follower_and_leader(self, conn_follower, conn_leader=None):
        if conn_leader is None:
            conn_leader = self.conn

        # Leader step down
        conn_leader.reconfigure(f'disaggregated=(role="follower")')

        meta = self.disagg_get_complete_checkpoint_meta(conn_leader)

        # Follower step up, including picking up the last complete checkpoint
        conn_follower.reconfigure(f'disaggregated=(checkpoint_meta="{meta}",' +\
                                  f'role="leader")')

    def reopen_disagg_conn(self, base_config, directory="."):
        """
        Reopen the connection.
        """
        config = base_config + f'disaggregated=(checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}"),'
        # Step down to avoid shutdown checkpoint
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.close_conn()
        self.open_conn(directory, config)
