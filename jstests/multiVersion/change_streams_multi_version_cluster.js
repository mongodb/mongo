// Verify that we can successfully resume a change stream during several different stages of a
// cluster upgrade.
//
// @tags: [uses_change_streams, requires_replication, fix_for_fcv_46]

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");       // Used by upgradeSet.
load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

const dbName = "test";
const collName = "change_streams_multi_version_sortkey";
const namespace = dbName + "." + collName;

// Start a sharded cluster in which all mongod and mongos processes are "last-stable" binVersion. We
// set "writePeriodicNoops" to write to the oplog every 1 second, which ensures that test change
// streams do not wait for longer than 1 second if one of the shards has no changes to report.
var st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 2,
        binVersion: "last-stable",
        setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
    },
    other: {mongosOptions: {binVersion: "last-stable"}}
});

let mongosConn = st.s;
assert.commandWorked(mongosConn.getDB(dbName).getCollection(collName).createIndex({shard: 1}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Shard the test collection and split it into two chunks: one that contains all {shard: 1}
// documents and one that contains all {shard: 2} documents.
st.shardColl(collName,
             {shard: 1} /* Shard key */,
             {shard: 2} /* Split at */,
             {shard: 2} /* Move the chunk containing {shard: 2} to its own shard */,
             dbName,
             true /* Wait until documents orphaned by the move get deleted */);

// Insert new documents on both shards, verify that each insertion outputs a result from the
// 'changeStream' cursor, verify that the change stream results have monotonically increasing
// timestamps, and return the resume token.
var nextId = 0;
function insertAndValidateChanges(coll, changeStream) {
    const docsToInsert =
        Array.from({length: 10}, (_, i) => ({_id: nextId + i, shard: i % 2, val: i}));
    nextId += docsToInsert.length;

    assert.commandWorked(coll.insert(docsToInsert));

    const changeList = [];
    assert.soon(function() {
        while (changeStream.hasNext()) {
            const change = changeStream.next();
            changeList.push(change);
        }

        return changeList.length === docsToInsert.length;
    }, changeList);

    for (let i = 0; i + 1 < changeList.length; ++i) {
        assert(timestampCmp(changeList[i].clusterTime, changeList[i + 1].clusterTime) <= 0,
               "Change timestamps are not monotonically increasing: " + tojson(changeList));
    }

    return changeStream.getResumeToken();
}

//
// Open and read a change stream on the "last-stable" cluster.
//
let coll = mongosConn.getDB(dbName)[collName];
let resumeToken = insertAndValidateChanges(coll, coll.watch());

//
// Upgrade the config db and the shards to the "latest" binVersion.
//
st.upgradeCluster("latest", {upgradeShards: true, upgradeConfigs: true, upgradeMongos: false});

//
// Open and read a change stream on the upgraded cluster but still using a "last-stable" version of
// mongos and "last-stable" for the FCV.
//
resumeToken = insertAndValidateChanges(coll, coll.watch([], {resumeAfter: resumeToken}));

//
// Upgrade mongos to the "latest" binVersion and then open and read a change stream, this time with
// all cluster nodes upgraded but still in "last-stable" FCV.
//
st.upgradeCluster("latest", {upgradeShards: false, upgradeConfigs: false, upgradeMongos: true});
mongosConn = st.s;
coll = mongosConn.getDB(dbName)[collName];

resumeToken = insertAndValidateChanges(coll, coll.watch([], {resumeAfter: resumeToken}));

//
// Set the FCV to the "latest" version, and then open and read a change stream on the completely
// upgraded cluster.
//
assert.commandWorked(mongosConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);
checkFCV(st.rs0.getPrimary().getDB("admin"), latestFCV);
checkFCV(st.rs1.getPrimary().getDB("admin"), latestFCV);

//
// Open and read a change stream on the upgraded cluster.
//
resumeToken = insertAndValidateChanges(coll, coll.watch([], {resumeAfter: resumeToken}));

st.stop();
}());
