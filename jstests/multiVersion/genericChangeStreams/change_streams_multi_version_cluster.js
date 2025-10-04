// Verify that we can successfully resume a change stream during several different stages of a
// cluster upgrade.
//
// @tags: [uses_change_streams, requires_replication]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

await import("jstests/multiVersion/libs/multi_rs.js");
await import("jstests/multiVersion/libs/multi_cluster.js");

const dbName = "test";
const collName = "change_streams_multi_version_sortkey";
const namespace = dbName + "." + collName;

function runTest(downgradeVersion) {
    jsTestLog("Running test with 'downgradeVersion': " + downgradeVersion);
    // Start a sharded cluster in which all mongod and mongos processes are of the downgraded
    // binVersion. We set "writePeriodicNoops" to write to the oplog every 1 second, which ensures
    // that test change streams do not wait for longer than 1 second if one of the shards has no
    // changes to report.
    let st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 2,
            binVersion: downgradeVersion,
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
        },
        other: {mongosOptions: {binVersion: downgradeVersion}},
        // By default, our test infrastructure sets the election timeout to a very high value (24
        // hours). For this test, we need a shorter election timeout because it relies on nodes
        // running an election when they do not detect an active primary. Therefore, we are setting
        // the electionTimeoutMillis to its default value.
        initiateWithDefaultElectionTimeout: true,
    });

    let mongosConn = st.s;
    assert.commandWorked(mongosConn.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    assert.commandWorked(mongosConn.getDB(dbName).getCollection(collName).createIndex({shard: 1}));

    // Shard the test collection and split it into two chunks: one that contains all {shard: 1}
    // documents and one that contains all {shard: 2} documents.
    st.shardColl(
        collName,
        {shard: 1} /* Shard key */,
        {shard: 2} /* Split at */,
        {shard: 2} /* Move the chunk containing {shard: 2} to its own shard */,
        dbName,
        true /* Wait until documents orphaned by the move get deleted */,
    );

    // Insert new documents on both shards, verify that each insertion outputs a result from the
    // 'changeStream' cursor, verify that the change stream results have monotonically increasing
    // timestamps, and return the resume token.
    let nextId = 0;
    function insertAndValidateChanges(coll, changeStream) {
        const docsToInsert = Array.from({length: 10}, (_, i) => ({_id: nextId + i, shard: i % 2, val: i}));
        nextId += docsToInsert.length;

        assert.commandWorked(coll.insert(docsToInsert));

        const changeList = [];
        assert.soon(function () {
            while (changeStream.hasNext()) {
                const change = changeStream.next();
                changeList.push(change);
            }

            return changeList.length === docsToInsert.length;
        }, changeList);

        for (let i = 0; i + 1 < changeList.length; ++i) {
            assert(
                timestampCmp(changeList[i].clusterTime, changeList[i + 1].clusterTime) <= 0,
                "Change timestamps are not monotonically increasing: " + tojson(changeList),
            );
        }

        return changeStream.getResumeToken();
    }

    //
    // Open and read a change stream on the downgrade version cluster.
    //
    let coll = mongosConn.getDB(dbName)[collName];
    let resumeToken = insertAndValidateChanges(coll, coll.watch());

    //
    // Upgrade the config db and the shards to the "latest" binVersion.
    //
    st.upgradeCluster("latest", {
        upgradeShards: true,
        upgradeConfigs: true,
        upgradeMongos: false,
        waitUntilStable: true,
    });

    //
    // Open and read a change stream on the upgraded cluster but still using a downgraded version of
    // mongos and downgraded version for the FCV.
    //
    resumeToken = insertAndValidateChanges(coll, coll.watch([], {resumeAfter: resumeToken}));

    //
    // Upgrade mongos to the "latest" binVersion and then open and read a change stream, this time
    // with all cluster nodes upgraded but still in downgraded FCV.
    //
    st.upgradeCluster("latest", {
        upgradeShards: false,
        upgradeConfigs: false,
        upgradeMongos: true,
        waitUntilStable: true,
    });
    mongosConn = st.s;
    coll = mongosConn.getDB(dbName)[collName];

    resumeToken = insertAndValidateChanges(coll, coll.watch([], {resumeAfter: resumeToken}));

    //
    // Set the FCV to the "latest" version, and then open and read a change stream on the completely
    // upgraded cluster.
    //
    assert.commandWorked(mongosConn.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);
    checkFCV(st.rs0.getPrimary().getDB("admin"), latestFCV);
    checkFCV(st.rs1.getPrimary().getDB("admin"), latestFCV);

    //
    // Open and read a change stream on the upgraded cluster.
    //
    resumeToken = insertAndValidateChanges(coll, coll.watch([], {resumeAfter: resumeToken}));

    st.stop();
}

runTest("last-continuous");
runTest("last-lts");
