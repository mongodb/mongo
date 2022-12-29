/**
 * Tests catalog shard topology.
 *
 * Requires both catalog shard feature flags.
 * @tags: [
 *   featureFlagCatalogShard,
 *   featureFlagConfigServerAlwaysShardRemote,
 * ]
 */
(function() {
"use strict";

const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;

const st = new ShardingTest({
    shards: 0,
    config: 3,
    configOptions: {
        setParameter:
            {featureFlagCatalogShard: true, featureFlagConfigServerAlwaysShardRemote: true}
    },
});

const configCS = st.configRS.getURL();

//
// Dedicated config server mode tests (pre addShard).
//
{
    // Can't create user namespaces.
    assert.commandFailedWithCode(st.configRS.getPrimary().getCollection(ns).insert({_id: 1, x: 1}),
                                 ErrorCodes.InvalidNamespace);

    // Failover works.
    st.configRS.stepUp(st.configRS.getSecondary());

    // Restart works. Restart all nodes to verify they don't rely on a majority of nodes being up.
    const configNodes = st.configRS.nodes;
    configNodes.forEach(node => {
        st.configRS.restart(node, undefined, undefined, false /* wait */);
    });
    st.configRS.getPrimary();  // Waits for a stable primary.
}

//
// Catalog shard mode tests (post addShard).
//
{
    //
    // Adding the config server as a shard works.
    //
    assert.commandWorked(st.s.adminCommand({addShard: configCS}));

    // More than once works.
    assert.commandWorked(st.s.adminCommand({addShard: configCS}));
    assert.commandWorked(st.s.adminCommand({addShard: configCS}));
}

// Refresh the logical session cache now that we have a shard to create the sessions collection to
// verify it works as expected.
st.configRS.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1});

st.stop();
}());
