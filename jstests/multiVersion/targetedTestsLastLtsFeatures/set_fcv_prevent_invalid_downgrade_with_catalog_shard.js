/**
 * Test to ensure that：
 *      1. The FCV cannot be downgraded to a version that does not have config shards if catalog
 *         shard is enabled.
 *      2. If the FCV does get downgraded to a version that does not support config shards, a
 *         config shard cannot be created (this can occur if an FCV downgrade happens concurrently
 *         with the creation of a config shard).
 *
 * @tags: [requires_fcv_70, featureFlagCatalogShard, featureFlagTransitionToCatalogShard]
 */
(function() {
"use strict";

// TODO (SERVER-74534): Enable the metadata consistency check when it will work with co-located
// configsvr.
TestData.skipCheckMetadataConsistency = true;

load("jstests/libs/config_shard_util.js");

const shardedNs = "foo.bar";
const unshardedNs = "unsharded_foo.unsharded_bar";

function basicCRUD(conn, ns) {
    assert.commandWorked(
        conn.getCollection(ns).insert([{_id: 1, x: 1, skey: -1000}, {_id: 2, skey: 1000}]));
    assert.sameMembers(conn.getCollection(ns).find().toArray(),
                       [{_id: 1, x: 1, skey: -1000}, {_id: 2, skey: 1000}]);
    assert.commandWorked(conn.getCollection(ns).remove({x: 1}));
    assert.commandWorked(conn.getCollection(ns).remove({skey: 1000}));
    assert.eq(conn.getCollection(ns).find().toArray().length, 0);
}

let splitPoint = 0;
function basicShardedDDL(conn, ns) {
    assert.commandWorked(conn.adminCommand({split: ns, middle: {skey: splitPoint}}));
    splitPoint += 10;
}

const st = new ShardingTest({shards: 2, configShard: true, other: {enableBalancer: true}});
const mongosAdminDB = st.s.getDB("admin");

assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: {skey: 1}}));

function runTest(targetFCV) {
    jsTest.log("Downgrading FCV to an unsupported version when configShard is enabled.");

    const errRes = assert.commandFailedWithCode(
        mongosAdminDB.runCommand({setFeatureCompatibilityVersion: targetFCV}),
        ErrorCodes.CannotDowngrade);
    assert.eq(errRes.errmsg,
          `Cannot downgrade featureCompatibilityVersion to ${targetFCV} with a config shard as it is not supported in earlier versions. Please transition the config server to dedicated mode using the transitionToDedicatedConfigServer command.`);

    // The downgrade fails and should not start the downgrade process on any cluster node.
    const configRes =
        st.config0.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert(configRes.featureCompatibilityVersion);
    assert.eq(configRes.featureCompatibilityVersion.version, latestFCV);

    const shardRes =
        st.shard1.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert(shardRes.featureCompatibilityVersion);
    assert.eq(shardRes.featureCompatibilityVersion.version, latestFCV);

    // The config shard's data can still be accessed.
    basicCRUD(st.s, shardedNs);
    basicShardedDDL(st.s, shardedNs);
    basicCRUD(st.s, unshardedNs);

    // Remove the config shard and verify we can now downgrade.
    ConfigShardUtil.transitionToDedicatedConfigServer(st);
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: targetFCV}));

    jsTest.log("Attempting to create a configShard on an unsupported FCV.");

    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: targetFCV}));
    assert.commandFailedWithCode(mongosAdminDB.runCommand({transitionFromDedicatedConfigServer: 1}),
                                 7467202);

    // Upgrade and transition back to config shard mode for the next test.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    assert.commandWorked(mongosAdminDB.runCommand({transitionFromDedicatedConfigServer: 1}));

    basicCRUD(st.s, shardedNs);
    basicShardedDDL(st.s, shardedNs);
    basicCRUD(st.s, unshardedNs);
}

runTest(lastLTSFCV);
runTest(lastContinuousFCV);

st.stop();
})();
