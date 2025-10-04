/**
 * Tests that read operations with readConcern majority only see committed data.
 *
 * The following read operations are tested:
 *  - find command
 *  - aggregation
 *  - distinct
 *  - count
 *
 * Each operation is tested on a single node, and (if supported) through mongos on both sharded and
 * unsharded collections. Mongos doesn't directly handle readConcern majority, but these tests
 * should ensure that it correctly propagates the setting to the shards when running commands.
 * This test requires a persistent storage engine because the makeSnapshot test command accesses
 * the oplog's record store.
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_sharding,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Skip metadata consistency checks since the sharded cluster is started with 0 shards
TestData.skipCheckMetadataConsistency = true;
TestData.skipCheckRoutingTableConsistency = true;

let testServer = MongoRunner.runMongod();
var db = testServer.getDB("test");
if (!db.serverStatus().storageEngine.supportsCommittedReads) {
    print("Skipping read_majority.js since storageEngine doesn't support it.");
    MongoRunner.stopMongod(testServer);
    quit();
}
MongoRunner.stopMongod(testServer);

function makeCursor(db, result) {
    return new DBCommandCursor(db, result);
}

// These test cases are functions that return a cursor of the documents in collections without
// fetching them yet.
let cursorTestCases = {
    find: function (coll) {
        return makeCursor(
            coll.getDB(),
            assert.commandWorked(coll.runCommand("find", {readConcern: {level: "majority"}, batchSize: 0})),
        );
    },
    aggregate: function (coll) {
        return makeCursor(
            coll.getDB(),
            assert.commandWorked(
                coll.runCommand("aggregate", {readConcern: {level: "majority"}, cursor: {batchSize: 0}, pipeline: []}),
            ),
        );
    },
    aggregateGeoNear: function (coll) {
        return makeCursor(
            coll.getDB(),
            assert.commandWorked(
                coll.runCommand("aggregate", {
                    readConcern: {level: "majority"},
                    cursor: {batchSize: 0},
                    pipeline: [{$geoNear: {near: [0, 0], distanceField: "d", spherical: true}}],
                }),
            ),
        );
    },
};

// These test cases have a run method that will be passed a collection with a single object with
// _id: 1 and a state field that equals either "before" or "after". The collection will also
// contain a 2dsphere index to enable testing commands that depend on it. The return value from the
// run method is expected to be the value of expectedBefore or expectedAfter depending on the state
// of the state field.
let nonCursorTestCases = {
    count_before: {
        run: function (coll) {
            let res = coll.runCommand("count", {readConcern: {level: "majority"}, query: {state: "before"}});
            assert.commandWorked(res);
            return res.n;
        },
        expectedBefore: 1,
        expectedAfter: 0,
    },
    count_after: {
        run: function (coll) {
            let res = coll.runCommand("count", {readConcern: {level: "majority"}, query: {state: "after"}});
            assert.commandWorked(res);
            return res.n;
        },
        expectedBefore: 0,
        expectedAfter: 1,
    },
    distinct: {
        run: function (coll) {
            let res = coll.runCommand("distinct", {readConcern: {level: "majority"}, key: "state"});
            assert.commandWorked(res);
            assert.eq(res.values.length, 1, tojson(res));
            return res.values[0];
        },
        expectedBefore: "before",
        expectedAfter: "after",
    },
};

function runTests(coll, mongodConnection) {
    function makeSnapshot() {
        return assert.commandWorked(mongodConnection.adminCommand("makeSnapshot")).name;
    }
    function setCommittedSnapshot(snapshot) {
        assert.commandWorked(mongodConnection.adminCommand({"setCommittedSnapshot": snapshot}));
    }

    assert.commandWorked(coll.createIndex({point: "2dsphere"}, {}, 0));
    for (var testName in cursorTestCases) {
        jsTestLog("Running " + testName + " against " + coll.toString());
        let getCursor = cursorTestCases[testName];

        // Setup initial state.
        assert.commandWorked(coll.remove({}));
        assert.commandWorked(coll.save({_id: 1, state: "before", point: [0, 0]}));
        setCommittedSnapshot(makeSnapshot());

        // Check initial conditions.
        assert.eq(getCursor(coll).next().state, "before");

        // Change state without making it committed.
        assert.commandWorked(coll.save({_id: 1, state: "after", point: [0, 0]}));

        // Cursor still sees old state.
        assert.eq(getCursor(coll).next().state, "before");

        // Create a cursor before the update is visible.
        let oldCursor = getCursor(coll);

        // Making a snapshot doesn't make the update visible yet.
        var snapshot = makeSnapshot();
        assert.eq(getCursor(coll).next().state, "before");

        // Setting it as committed does for both new and old cursors.
        setCommittedSnapshot(snapshot);
        assert.eq(getCursor(coll).next().state, "after");
        assert.eq(oldCursor.next().state, "after");
    }

    for (var testName in nonCursorTestCases) {
        jsTestLog("Running " + testName + " against " + coll.toString());
        let getResult = nonCursorTestCases[testName].run;
        let expectedBefore = nonCursorTestCases[testName].expectedBefore;
        let expectedAfter = nonCursorTestCases[testName].expectedAfter;

        // Setup initial state.
        assert.commandWorked(coll.remove({}));
        assert.commandWorked(coll.save({_id: 1, state: "before", point: [0, 0]}));
        setCommittedSnapshot(makeSnapshot());

        // Check initial conditions.
        assert.eq(getResult(coll), expectedBefore);

        // Change state without making it committed.
        assert.commandWorked(coll.save({_id: 1, state: "after", point: [0, 0]}));

        // Cursor still sees old state.
        assert.eq(getResult(coll), expectedBefore);

        // Making a snapshot doesn't make the update visible yet.
        var snapshot = makeSnapshot();
        assert.eq(getResult(coll), expectedBefore);

        // Setting it as committed does.
        setCommittedSnapshot(snapshot);
        assert.eq(getResult(coll), expectedAfter);
    }
}

let replTest = new ReplSetTest({
    nodes: 1,
    oplogSize: 2,
    nodeOptions: {setParameter: "testingSnapshotBehaviorInIsolation=true", shardsvr: ""},
});
replTest.startSet();
// Cannot wait for a stable recovery timestamp with 'testingSnapshotBehaviorInIsolation' set.
replTest.initiate(null, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});

let mongod = replTest.getPrimary();

// Do a majority write to wait for a valid committed snapshot before starting the test. This is
// needed to make sure no oplog holes behind the clusterTime and all internal writes as part of the
// server startup are committed. Otherwise, manually setting the committed snapshot to the latest
// clusterTime using the `setCommittedSnapshot` command could result in reading ahead of the
// all_durable.
assert.commandWorked(mongod.getDB("test")["coll"].insert({x: 1}, {writeConcern: {w: "majority"}}));

(function testSingleNode() {
    let db = mongod.getDB("singleNode");
    runTests(db.collection, mongod);
})();

let shardingTest = new ShardingTest({
    shards: 0,
    mongos: 1,
});
assert(shardingTest.adminCommand({addShard: replTest.getURL()}));

(function testUnshardedDBThroughMongos() {
    let db = shardingTest.getDB("throughMongos");
    runTests(db.unshardedDB, mongod);
})();

shardingTest.adminCommand({enableSharding: "throughMongos"});

(function testUnshardedCollectionThroughMongos() {
    let db = shardingTest.getDB("throughMongos");
    runTests(db.unshardedCollection, mongod);
})();

(function testShardedCollectionThroughMongos() {
    let db = shardingTest.getDB("throughMongos");
    let collection = db.shardedCollection;
    shardingTest.adminCommand({shardCollection: collection.getFullName(), key: {_id: 1}});
    runTests(collection, mongod);
})();

shardingTest.stop();
replTest.stopSet();
