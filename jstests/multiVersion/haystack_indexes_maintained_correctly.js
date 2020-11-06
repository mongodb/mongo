/**
 * Verifies that haystack indexes cannot be created on 4.9+ binaries, but are maintained
 * correctly in mixed version clusters.
 *
 * TODO SERVER-51871: This test can be deleted once 5.0 becomes last-lts.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");      // For 'upgradeSecondaries()/upgradePrimary()'
load('jstests/noPassthrough/libs/index_build.js');  // For 'assertIndexes()'
load("jstests/libs/fixture_helpers.js");            // For 'isSharded()'

const dbName = "test";
const collName = jsTestName();
const geoIndexKey = {
    loc: "geoHaystack",
    x: 1
};
const geoIndexName = "geo";
const indexList = ["_id_", geoIndexName];
const nonGeoIndexList = ["_id_"];

function insertInitialDocuments(coll) {
    const documentList =
        [{_id: 0, loc: [1, 2], x: 'foo', y: 2}, {_id: 1, loc: [1.5, 1.5], x: 'bar', y: 1}];
    assert.commandWorked(coll.insert(documentList));
}

/**
 * Calls 'validate()' on 'coll' and verifies that the documents which are inserted into 'coll'
 * produce the correct number of index keys for the geoHaystack index in this test.
 */
function validateAndAssertCorrectIndexKeys(coll) {
    const validateResult = assert.commandWorked(coll.validate({full: true}));
    let validateOutput;
    if (FixtureHelpers.isSharded(coll)) {
        assert(validateResult.hasOwnProperty("raw"));
        validateOutput = validateResult["raw"];
    } else {
        validateOutput = validateResult;
    }

    // There should be as many index keys as there are documents.
    const expectedNumKeys = coll.find().itcount();
    let keys = 0;
    if (FixtureHelpers.isSharded(coll)) {
        for (const shard of Object.keys(validateOutput)) {
            keys += validateOutput[shard]["keysPerIndex"][geoIndexName];
            assert.eq(0, validateOutput[shard]["errors"].length);
            assert.eq(true, validateOutput[shard]["valid"]);
        }
    } else {
        keys = validateOutput["keysPerIndex"][geoIndexName];
        assert.eq(0, validateOutput["errors"].length);
        assert.eq(true, validateOutput["valid"]);
    }

    assert.eq(expectedNumKeys, keys);
}

// Verify that haystack indexes cannot be created on a standalone in the latest version regardless
// of the FCV.
function runStandaloneTest() {
    const mongo = MongoRunner.runMongod({binVersion: "latest"});
    const testDB = mongo.getDB(dbName);
    const coll = testDB[collName];
    for (const fcv of [lastLTSFCV, latestFCV]) {
        assert.commandWorked(mongo.adminCommand({setFeatureCompatibilityVersion: fcv}));
        assert.commandFailedWithCode(
            coll.createIndex(geoIndexKey, {name: geoIndexName, bucketSize: 1}),
            ErrorCodes.CannotCreateIndex);
    }
    MongoRunner.stopMongod(mongo);
}

// Verify that haystack indexes are maintained properly on a mixed version replica set.
function runReplicaSetTest() {
    // Set up a mixed version replica-set: both nodes will be initialized to the last-lts
    // binary version (4.4), but the secondary will be initialized to the latest binary version.
    const rst = new ReplSetTest({nodes: [{binVersion: "last-lts"}, {binVersion: "latest"}]});
    rst.startSet();
    rst.initiate();
    let primaryDB = rst.getPrimary().getDB(dbName);
    let primaryColl = primaryDB[collName];
    insertInitialDocuments(primaryColl);
    rst.awaitReplication();

    // Creating a haystack index should still work.
    assert.commandWorked(primaryDB.runCommand({
        "createIndexes": collName,
        indexes: [{key: geoIndexKey, name: geoIndexName, bucketSize: 1}],
        writeConcern: {w: 2}
    }));

    // The haystack index should replicate correctly to the secondary.
    const secondaryDB = rst.getSecondary().getDB(dbName);
    const secondaryColl = secondaryDB[collName];
    IndexBuildTest.assertIndexes(secondaryColl, indexList.length, indexList);

    // Verify that documents which are inserted after the index is built produce valid index keys.
    assert.commandWorked(
        primaryColl.insert([{_id: 4, loc: [4, 4], x: "baz"}, {_id: 5, loc: [5, 5], x: "baz"}],
                           {writeConcern: {w: 2}}));
    validateAndAssertCorrectIndexKeys(primaryColl);
    validateAndAssertCorrectIndexKeys(secondaryColl);

    // Upgrade the primary and attempt to re-create the index after the upgrade.
    assert.commandWorked(
        primaryDB.runCommand({"dropIndexes": collName, index: geoIndexName, writeConcern: {w: 2}}));
    rst.upgradePrimary(rst.getPrimary(), {binVersion: "latest"});
    rst.awaitNodesAgreeOnPrimary();

    // Even though we haven't bumped the FCV, index creation should still fail on a primary in
    // the latest version.
    primaryDB = rst.getPrimary().getDB(dbName);
    primaryColl = primaryDB[collName];
    assert.commandFailedWithCode(
        primaryColl.createIndex(geoIndexKey, {name: geoIndexName, bucketSize: 1}),
        ErrorCodes.CannotCreateIndex);

    rst.stopSet();
}

// Verify that haystack indexes are maintained properly in a mixed version sharded cluster.
function runShardingTest() {
    // Set up a mixed version sharded cluster, where shard0's nodes are initialized to the last-lts
    // binary version (4.4) and shard1's nodes are initialized to the latest binary version.
    const st = new ShardingTest({
        shards: {
            rs0: {nodes: [{binVersion: "last-lts"}, {binVersion: "last-lts"}]},
            rs1: {nodes: [{binVersion: "latest"}, {binVersion: "latest"}]}
        },
        other: {mongosOptions: {binVersion: "last-lts"}}
    });

    // Test that indexes are maintained properly during chunk migration. More precisely, verify
    // that when a chunk from a shard consisting of 4.4 nodes with a haystack index is moved to a
    // shard consisting of nodes in the latest binary version, the haystack index is built
    // correctly on the shard in the latest binary version.
    const mongos = st.s;
    const ns = dbName + "." + collName;

    // Create a sharded collection with two chunks: [MinKey, 1), [1, MaxKey], both on shard0.
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 1}}));

    // Insert some documents and create a haystack index.
    const db = mongos.getDB(dbName);
    const coll = db[collName];
    insertInitialDocuments(coll);
    assert.commandWorked(coll.createIndex(geoIndexKey, {name: geoIndexName, bucketSize: 1}));

    // Wait for shard0 to finish replicating its documents and building the index.
    st.rs0.awaitReplication();

    // Move the [1, MaxKey] chunk to shard1.
    assert.commandWorked(mongos.adminCommand(
        {moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}));
    st.rs1.awaitReplication();

    // Verify that shard1 has the haystack index after the chunk was moved.
    const shard1primary = st.rs1.getPrimary();
    const shard1DB = shard1primary.getDB(dbName);
    const shard1Coll = shard1DB[collName];
    IndexBuildTest.assertIndexes(shard1Coll, indexList.length, indexList);

    validateAndAssertCorrectIndexKeys(coll);

    // Verify that inserting documents into a shard consisting of nodes in the latest version with
    // an existing haystack index will create the correct index keys for the index.
    assert.commandWorked(
        coll.insert([{_id: 4, loc: [4, 4], x: "baz"}, {_id: 5, loc: [5, 5], x: "blah"}],
                    {writeConcern: {w: 2}}));

    validateAndAssertCorrectIndexKeys(coll);

    // Creating a new haystack index against a sharded cluster with at least one shard upgraded to
    // the latest version should fail.
    assert.commandWorked(
        db.runCommand({"dropIndexes": collName, index: geoIndexName, writeConcern: {w: 2}}));
    assert.commandFailedWithCode(coll.createIndex(geoIndexKey, {name: geoIndexName, bucketSize: 1}),
                                 ErrorCodes.CannotCreateIndex);

    // Though the command failed, the haystack index will still be created on shard0 since it is in
    // version 4.4.
    const shard0DB = st.rs0.getPrimary().getDB(dbName);
    const shard0coll = shard0DB[collName];
    IndexBuildTest.assertIndexes(shard0coll, indexList.length, indexList);

    // Since shard1 is in the latest version, it will not have the geoHaystack index.
    IndexBuildTest.assertIndexes(shard1Coll, nonGeoIndexList.length, nonGeoIndexList);

    // Though the 'dropIndexes' command will fail because shard1 does not have the haystack
    // index, it should still remove the haystack index on shard0.
    assert.commandFailedWithCode(
        mongos.getDB(dbName).runCommand(
            {"dropIndexes": collName, index: geoIndexName, writeConcern: {w: 2}}),
        ErrorCodes.IndexNotFound);
    IndexBuildTest.assertIndexes(shard0coll, nonGeoIndexList.length, nonGeoIndexList);
    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();
})();