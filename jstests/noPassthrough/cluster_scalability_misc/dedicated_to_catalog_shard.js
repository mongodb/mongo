/**
 * Tests config shard topology.
 *
 * @tags: [
 *   requires_fcv_80,
 *   config_shard_incompatible,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;

function flushRoutingAndDBCacheUpdates(conn) {
    if (!FeatureFlagUtil.isPresentAndEnabled(conn, "ShardAuthoritativeDbMetadataCRUD")) {
        assert.commandWorked(conn.adminCommand({_flushDatabaseCacheUpdates: dbName}));
        assert.commandWorked(conn.adminCommand({_flushDatabaseCacheUpdates: "notRealDB"}));
    }

    assert.commandWorked(conn.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(conn.adminCommand({_flushRoutingTableCacheUpdates: "does.not.exist"}));
}

const st = new ShardingTest({
    shards: 0,
    config: 3,
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

// Dedicated config server mode tests (pre addShard).
{
    // Can create user namespaces via direct writes.
    assert.commandWorked(st.configRS.getPrimary().getCollection(ns).insert({_id: 1, x: 1}));

    // Failover works.
    st.configRS.stepUp(st.configRS.getSecondary());
    st.configRS.awaitReplication();

    // Restart works. Restart all nodes to verify they don't rely on a majority of nodes being up.
    // On Windows, restart sequentially to avoid port binding races due to TCP TIME_WAIT (BF-42230).
    const configNodes = st.configRS.nodes;
    const waitForConnect = _isWindows();
    configNodes.forEach((node) => {
        st.configRS.restart(node, undefined, undefined, waitForConnect);
    });
    st.configRS.getPrimary(); // Waits for a stable primary.

    // Flushing routing / db cache updates works.
    flushRoutingAndDBCacheUpdates(st.configRS.getPrimary());
}

// Config shard mode tests (post addShard).
{
    // Dropping user database as only empty replicasets are allowed to add
    assert.commandWorked(st.configRS.getPrimary().getDB(dbName).dropDatabase());

    // Adding the config server as a shard works.
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

    // More than once works.
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

    // Flushing routing / db cache updates works.
    flushRoutingAndDBCacheUpdates(st.configRS.getPrimary());
}

// Refresh the logical session cache now that we have a shard to create the sessions collection to
// verify it works as expected.
st.configRS.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1});

let shardList = st.getDB("config").shards.find().toArray();
assert.eq(1, shardList.length, tojson(shardList));

st.stop();
