// Test that upgrade downgrade transfers the create collection coordinator for
// config.system.sessions collection between the shard and config server.

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

function dropSessionsCollectionManually(st) {
    let collDoc =
        st.s.getDB("config").getCollection("collections").findOne({_id: "config.system.sessions"});
    if (collDoc == undefined) {
        return;
    }
    let uuid = collDoc.uuid;
    assert.commandWorked(st.s.getDB("config").getCollection("collections").deleteOne({
        _id: "config.system.sessions"
    }));
    assert.commandWorked(st.s.getDB("config").getCollection("chunks").deleteMany({uuid: uuid}));
    st.configRS.getPrimary().getDB("config").getCollection("system.sessions").drop();
    st.shard0.getDB("config").getCollection("system.sessions").drop();
    st.shard1.getDB("config").getCollection("system.sessions").drop();
}

function checkSessionsCollectionPresent(conn, expectPresent) {
    let result =
        conn.getDB("config").runCommand({listCollections: 1, filter: {name: "system.sessions"}});
    let expectedLength = expectPresent ? 1 : 0;
    assert.eq(result.cursor.firstBatch.length, expectedLength);
}

function createSessionsCollectionAndCheck(configSvrConnection, expectPresent) {
    assert.commandWorked(configSvrConnection.adminCommand({refreshLogicalSessionCacheNow: 1}));
    checkSessionsCollectionPresent(configSvrConnection, expectPresent);
}

jsTest.log("Dedicated config server tests");
{
    const st = new ShardingTest({
        shards: 2,
        other: {configOptions: {setParameter: {disableLogicalSessionCacheRefresh: true}}},
    });

    jsTest.log("Basic test case in new FCV");
    dropSessionsCollectionManually(st);
    createSessionsCollectionAndCheck(st.configRS.getPrimary(), true);

    jsTest.log("Test that downgrade drops the collection on the config server");
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    checkSessionsCollectionPresent(st.configRS.getPrimary(), false);

    jsTest.log("Basic test in old FCV");
    dropSessionsCollectionManually(st);
    createSessionsCollectionAndCheck(st.configRS.getPrimary(), false);

    jsTest.log("Test that upgrade creates the collection on the config server");
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    checkSessionsCollectionPresent(st.configRS.getPrimary(), true);

    jsTest.log("Downgrade again so we can do more upgrade tests");
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    jsTest.log("Upgrading will create the sessions collection if it can (before upgrading)");
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    checkSessionsCollectionPresent(st.configRS.getPrimary(), true);

    jsTest.log("Fail downgrade early so we can re-upgrade");
    let failFCV = configureFailPoint(st.configRS.getPrimary(), "failDowngrading");
    assert.commandFailedWithCode(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 549181);
    failFCV.off();
    dropSessionsCollectionManually(st);
    createSessionsCollectionAndCheck(st.configRS.getPrimary(), false);

    jsTest.log("Re-upgrading should create the collection on the config server");
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    checkSessionsCollectionPresent(st.configRS.getPrimary(), true);

    st.stop();
}

jsTest.log("Embedded config server tests");
{
    const st = new ShardingTest({
        shards: 2,
        configShard: true,
        other: {configOptions: {setParameter: {disableLogicalSessionCacheRefresh: true}}},
    });

    jsTest.log("Basic test case in new FCV");
    dropSessionsCollectionManually(st);
    createSessionsCollectionAndCheck(st.configRS.getPrimary(), true);

    st.startBalancer();

    jsTest.log("Transition to dedicated shouldn't drop the collection");
    removeShard(st, "config");
    checkSessionsCollectionPresent(st.configRS.getPrimary(), true);

    jsTest.log("Transition from dedicated shouldn't drop the collection");
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
    checkSessionsCollectionPresent(st.configRS.getPrimary(), true);
    st.stopBalancer();

    jsTest.log("Downgrade to test config transitions on lower FCV");
    dropSessionsCollectionManually(st);
    createSessionsCollectionAndCheck(st.configRS.getPrimary(), true);
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    // Expect the chunk to be on the config server so downgrade shouldn't drop the collection.
    checkSessionsCollectionPresent(st.configRS.getPrimary(), true);

    jsTest.log("Transition to dedicated should drop the collection");
    st.startBalancer();
    removeShard(st, "config");
    checkSessionsCollectionPresent(st.configRS.getPrimary(), false);

    jsTest.log("Transition from dedicated should drop the collection");
    assert.commandWorked(
        st.configRS.getPrimary().getDB("config").createCollection("system.sessions"));
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
    checkSessionsCollectionPresent(st.configRS.getPrimary(), false);

    st.stop();
}
