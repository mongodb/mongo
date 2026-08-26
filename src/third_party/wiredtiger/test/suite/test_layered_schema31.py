#!/usr/bin/env python3
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

# Step-up creates a missing stable table from its recorded configuration when
# available; legacy mode derives the configuration from ingest metadata.

from helper_disagg import DisaggSchemaEpochMixin
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wttest import WiredTigerTestCase, open_cursor

@disagg_test_class
class test_layered_schema31(WiredTigerTestCase, DisaggSchemaEpochMixin):
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'

    # Exercise every user-settable field except encryption, which requires an
    # encryptor extension.
    base_config = (
        "key_format=i,value_format=S,"
        "allocation_size=512B,"
        "block_allocation=first,"
        "block_compressor=snappy,"
        "block_manager=disagg,"
        "checksum=unencrypted,"
        "dictionary=100,"
        "disaggregated=(storage_tier=cold),"
        "internal_key_truncate=false,"
        "internal_page_max=8KB,"
        "key_gap=20,"
        "leaf_key_max=256,"
        "leaf_page_max=8KB,"
        "leaf_value_max=1KB,"
        "memory_page_image_max=64KB,"
        "memory_page_max=1MB,"
        "prefix_compression=true,"
        "prefix_compression_min=8,"
        "split_deepen_min_child=100,"
        "split_deepen_per_child=50,"
        "split_pct=80"
    )

    uri_scenarios = [
        (
            "layered",
            {
                "uri": f"layered:{__qualname__}",
                "config": base_config,
            },
        ),
        (
            "table",
            {
                "uri": f"table:{__qualname__}",
                "config": base_config + ",type=layered",
            },
        ),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uri_scenarios)

    def conn_extensions(self, extlist):
        self.add_scenario_config()
        extlist.extension("compressors", "snappy")
        return self.disagg_conn_extensions(extlist)

    def create_on_follower_then_step_up(self, config, use_schema_epochs=True):
        """
        Create the table in the follower role, then step up to create the
        missing stable table.
        """
        self.step_down()

        if use_schema_epochs:
            self.set_stable_epoch(1)

        # A follower create leaves the stable table missing until step-up.
        self.session.create(self.uri, config)

        if use_schema_epochs:
            self.publish(self.uri, epoch=5)

        self.step_up()

    def read_stable_config(self, layered_uri):
        """Return the stable configuration for a layered table."""
        stable_uri = "file:" + layered_uri.split(":", 1)[1] + ".wt_stable"
        with open_cursor(self.session, "metadata:create") as cursor:
            return cursor[stable_uri]

    def check_step_up_stable_config(self, config):
        """
        Check that step-up creates the stable table with the expected
        configuration.
        """
        # Get the config produced when the leader creates the stable table.
        leader_table_uri = self.uri + "_leader"
        self.session.create(leader_table_uri, config)
        expected_config = self.read_stable_config(leader_table_uri)

        # Compare it with the config of the stable table created at step-up.
        step_up_config = self.read_stable_config(self.uri)
        self.assertEqual(step_up_config, expected_config)

    def test_schema_epoch_step_up_matches_leader_config(self):
        """
        Verify that schema epoch step-up matches the leader's stable
        configuration.
        """
        self.create_on_follower_then_step_up(self.config)
        self.check_step_up_stable_config(self.config)

    def test_legacy_step_up_derives_config_from_ingest(self):
        """
        Verify that legacy step-up derives the stable configuration from ingest
        metadata.
        """
        self.create_on_follower_then_step_up(
            self.config, use_schema_epochs=False
        )
        self.check_step_up_stable_config(self.config)

    def test_schema_epoch_step_up_does_not_derive_from_ingest(self):
        """
        Verify that schema epoch step-up does not derive the stable
        configuration from ingest metadata.
        """
        # Deriving the configuration from ingest metadata forces
        # block_manager=disagg. If block_manager=default is found after
        # step-up, it proves the ingest-derived path was not used.
        table_config = self.config.replace(
            "block_manager=disagg", "block_manager=default"
        )
        self.create_on_follower_then_step_up(table_config)
        self.check_step_up_stable_config(table_config)
