/**
 * Verifies that haystack indexes are removed during the upgrade process from 4.4 to 4.9 for a
 * standalone and a replica set.
 *
 * TODO SERVER-51871: Since this test is specific to the upgrade process from 4.4 to 4.9/5.0, it
 * can be deleted once 5.0 becomes last-lts.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");         // For 'upgradeSet()'
load("jstests/multiVersion/libs/multi_cluster.js");    // For 'upgradeCluster()'
load('jstests/multiVersion/libs/verify_versions.js');  // For 'assert.binVersion()'
load('jstests/noPassthrough/libs/index_build.js');     // For 'assertIndexes()'

const dbName = "test";
const collName = jsTestName();
const geoIndexKey = {
    loc: "geoHaystack",
    x: 1
};
const geoIndexName = "geo";
const nonGeoIndexKey = {
    y: 1
};
const nonGeoIndexName = "y";

// Set of indexes to insert.
const indexList = ["_id_", nonGeoIndexName, geoIndexName];

// Set of indexes that will be present after upgrade is complete.
const nonGeoIndexList = ["_id_", nonGeoIndexName];

function insertDocuments(coll) {
    const documentList =
        [{_id: 0, loc: [1, 2], x: 'foo', y: 2}, {_id: 1, loc: [1.5, 1.5], x: 'bar', y: 1}];
    assert.commandWorked(coll.insert(documentList));
}

function createIndexes(coll) {
    assert.commandWorked(coll.createIndex(geoIndexKey, {name: geoIndexName, bucketSize: 1}));
    assert.commandWorked(coll.createIndex(nonGeoIndexKey, {name: nonGeoIndexName}));
}

// Verify that haystack indexes are deleted when upgrading a standalone.
function runStandaloneTest() {
    // Set up a v4.4 mongod.
    const dbPath = MongoRunner.dataPath + "/delete_haystack";
    let mongo = MongoRunner.runMongod({dbpath: dbPath, binVersion: "last-lts"});
    assert.neq(null, mongo, "mongod was unable to start up");
    let testDB = mongo.getDB(dbName);
    let coll = testDB[collName];
    insertDocuments(coll);
    createIndexes(coll);
    IndexBuildTest.assertIndexes(testDB[collName], indexList.length, indexList);
    MongoRunner.stopMongod(mongo);

    // Restart the mongod in the latest version.
    mongo = MongoRunner.runMongod(
        {dbpath: dbPath, binVersion: "latest", restart: true, cleanData: false});
    assert.neq(null, mongo, "mongod was unable to start up");
    testDB = mongo.getDB(dbName);
    coll = testDB[collName];

    // The haystack index should still be present before the FCV is set and the validate command
    // should succeed.
    IndexBuildTest.assertIndexes(coll, indexList.length, indexList);
    const validate = assert.commandWorked(coll.validate({full: true}));
    assert.eq(true, validate.valid);

    // Set the FCV.
    const adminDB = mongo.getDB("admin");
    checkFCV(adminDB, lastLTSFCV);
    assert.commandWorked(mongo.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);

    // The haystack index should no longer be present after the FCV is set.
    IndexBuildTest.assertIndexes(coll, nonGeoIndexList.length, nonGeoIndexList);
    MongoRunner.stopMongod(mongo);
}

/**
 * Verifies that every node in 'replSetTest' has the indexes in 'expectedIndexes'.
 */
function verifyIndexesPresentOnAllNodes(replSetTest, expectedIndexes, expectedPrimary) {
    // Make sure that the replica set is stable.
    if (expectedPrimary) {
        replSetTest.awaitNodesAgreeOnPrimary(
            replSetTest.kDefaultTimeoutMS, replSetTest.nodes, expectedPrimary);
    } else {
        replSetTest.awaitNodesAgreeOnPrimary();
    }
    for (const node of [replSetTest.getPrimary(), replSetTest.getSecondary()]) {
        const db = node.getDB(dbName);
        const coll = db[collName];
        IndexBuildTest.assertIndexes(coll, expectedIndexes.length, expectedIndexes);
    }
}

// Verify that haystack indexes get deleted when upgrading a replica set.
function runReplicaSetTest() {
    // Set up a replica-set in v4.4.
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: "last-lts"}});
    rst.startSet();
    rst.initiate();

    const initialPrimary = rst.getPrimary();
    const primaryDB = initialPrimary.getDB(dbName);
    const primaryColl = primaryDB[collName];
    insertDocuments(primaryColl);
    createIndexes(primaryColl);

    // Wait until both nodes finish inserting the documents and building the index.
    rst.awaitReplication();

    verifyIndexesPresentOnAllNodes(rst, indexList, initialPrimary);

    // Upgrade the secondary.
    rst.upgradeSecondaries({binVersion: "latest"});

    // Verify that the primary has not changed and is in the last-lts version, while the
    // secondary is in the latest version.
    assert.eq(initialPrimary, rst.getPrimary());
    assert.binVersion(initialPrimary, "last-lts");
    assert.binVersion(rst.getSecondary(), "latest");

    verifyIndexesPresentOnAllNodes(rst, indexList, initialPrimary);

    // Upgrade the primary.
    const upgradedPrimary = rst.upgradePrimary(initialPrimary, {binVersion: "latest"});

    // Verify that all nodes are in the latest version.
    for (const node of rst.nodes) {
        assert.binVersion(node, "latest");
    }

    verifyIndexesPresentOnAllNodes(rst, indexList, upgradedPrimary);

    // Set the FCV.
    const adminDB = upgradedPrimary.getDB("admin");
    checkFCV(adminDB, lastLTSFCV);
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);

    // Neither the primary nor the secondary should have the haystack index.
    verifyIndexesPresentOnAllNodes(rst, nonGeoIndexList);

    rst.stopSet();
}

// Even though the 'geoSearch' command is not allowed on sharded clusters, haystack indexes can
// still be created on a sharded cluster. As such, we verify that haystack indexes get deleted
// when upgrading a sharded cluster.
function runShardingTest() {
    // Set up a sharded cluster in v4.4.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2, binVersion: "last-lts"},
        other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
    });

    let mongos = st.s;
    const ns = dbName + "." + collName;

    // Create a sharded collection with two chunks, one on each shard: [-inf, 1), [1, inf). This
    // guarantees that each shard will have one of the two documents being inserted and both shards
    // will create a geoHaystack index.
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 1}}));

    // Move the [1, inf) chunk to shard 1.
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}));

    const db = mongos.getDB(dbName);
    const coll = db[collName];
    insertDocuments(coll);
    createIndexes(coll);

    // Wait for both shards to finish replicating their document and building the indexes.
    st.rs0.awaitReplication();
    st.rs1.awaitReplication();

    /**
     * Verify that each shard has each index in 'expectedIndexes'.
     */
    function verifyIndexesOnAllShards(expectedIndexes) {
        for (const shard of [st.rs0, st.rs1]) {
            verifyIndexesPresentOnAllNodes(shard, expectedIndexes);
        }
    }

    verifyIndexesOnAllShards(indexList);

    // Upgrade the shards and the config servers.
    st.upgradeCluster("latest", {upgradeShards: true, upgradeConfigs: true, upgradeMongos: false});
    st.waitUntilStable();

    // Indexes should still be present.
    verifyIndexesOnAllShards(indexList);

    // Upgrade the mongos.
    st.upgradeCluster("latest", {upgradeShards: false, upgradeConfigs: false, upgradeMongos: true});
    st.waitUntilStable();

    /**
     * Verifies that the FCV across 'st' matches 'targetFCV'.
     */
    function checkClusterFCV(targetFCV) {
        checkFCV(st.configRS.getPrimary().getDB("admin"), targetFCV);
        checkFCV(st.shard0.getDB("admin"), targetFCV);
        checkFCV(st.shard1.getDB("admin"), targetFCV);
    }

    // Set the FCV.
    mongos = st.s;
    const adminDB = mongos.getDB("admin");
    checkClusterFCV(lastLTSFCV);
    verifyIndexesOnAllShards(indexList);
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkClusterFCV(latestFCV);

    // None of the nodes in the cluster should have the haystack index.
    verifyIndexesOnAllShards(nonGeoIndexList);

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();
})();