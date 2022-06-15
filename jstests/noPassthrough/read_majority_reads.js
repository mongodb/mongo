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
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_sharding,
 * ]
 */

(function() {
'use strict';

var testServer = MongoRunner.runMongod();
var db = testServer.getDB("test");
if (!db.serverStatus().storageEngine.supportsCommittedReads) {
    print("Skipping read_majority.js since storageEngine doesn't support it.");
    MongoRunner.stopMongod(testServer);
    return;
}
MongoRunner.stopMongod(testServer);

function makeCursor(db, result) {
    return new DBCommandCursor(db, result);
}

// These test cases are functions that return a cursor of the documents in collections without
// fetching them yet.
var cursorTestCases = {
    find: function(coll) {
        return makeCursor(coll.getDB(),
                          assert.commandWorked(coll.runCommand(
                              'find', {readConcern: {level: 'majority'}, batchSize: 0})));
    },
    aggregate: function(coll) {
        return makeCursor(
            coll.getDB(),
            assert.commandWorked(coll.runCommand(
                'aggregate',
                {readConcern: {level: 'majority'}, cursor: {batchSize: 0}, pipeline: []})));
    },
    aggregateGeoNear: function(coll) {
        return makeCursor(coll.getDB(), assert.commandWorked(coll.runCommand('aggregate', {
            readConcern: {level: 'majority'},
            cursor: {batchSize: 0},
            pipeline: [{$geoNear: {near: [0, 0], distanceField: "d", spherical: true}}]
        })));
    },
};

// These test cases have a run method that will be passed a collection with a single object with
// _id: 1 and a state field that equals either "before" or "after". The collection will also
// contain a 2dsphere index to enable testing commands that depend on it. The return value from the
// run method is expected to be the value of expectedBefore or expectedAfter depending on the state
// of the state field.
var nonCursorTestCases = {
    count_before: {
        run: function(coll) {
            var res = coll.runCommand('count',
                                      {readConcern: {level: 'majority'}, query: {state: 'before'}});
            assert.commandWorked(res);
            return res.n;
        },
        expectedBefore: 1,
        expectedAfter: 0,
    },
    count_after: {
        run: function(coll) {
            var res = coll.runCommand('count',
                                      {readConcern: {level: 'majority'}, query: {state: 'after'}});
            assert.commandWorked(res);
            return res.n;
        },
        expectedBefore: 0,
        expectedAfter: 1,
    },
    distinct: {
        run: function(coll) {
            var res = coll.runCommand('distinct', {readConcern: {level: 'majority'}, key: 'state'});
            assert.commandWorked(res);
            assert.eq(res.values.length, 1, tojson(res));
            return res.values[0];
        },
        expectedBefore: 'before',
        expectedAfter: 'after',
    },
};

function runTests(coll, mongodConnection) {
    function makeSnapshot() {
        return assert.commandWorked(mongodConnection.adminCommand("makeSnapshot")).name;
    }
    function setCommittedSnapshot(snapshot) {
        assert.commandWorked(mongodConnection.adminCommand({"setCommittedSnapshot": snapshot}));
    }

    assert.commandWorked(coll.createIndex({point: '2dsphere'}, {}, 0));
    for (var testName in cursorTestCases) {
        jsTestLog('Running ' + testName + ' against ' + coll.toString());
        var getCursor = cursorTestCases[testName];

        // Setup initial state.
        assert.commandWorked(coll.remove({}));
        assert.commandWorked(coll.save({_id: 1, state: 'before', point: [0, 0]}));
        setCommittedSnapshot(makeSnapshot());

        // Check initial conditions.
        assert.eq(getCursor(coll).next().state, 'before');

        // Change state without making it committed.
        assert.commandWorked(coll.save({_id: 1, state: 'after', point: [0, 0]}));

        // Cursor still sees old state.
        assert.eq(getCursor(coll).next().state, 'before');

        // Create a cursor before the update is visible.
        var oldCursor = getCursor(coll);

        // Making a snapshot doesn't make the update visible yet.
        var snapshot = makeSnapshot();
        assert.eq(getCursor(coll).next().state, 'before');

        // Setting it as committed does for both new and old cursors.
        setCommittedSnapshot(snapshot);
        assert.eq(getCursor(coll).next().state, 'after');
        assert.eq(oldCursor.next().state, 'after');
    }

    for (var testName in nonCursorTestCases) {
        jsTestLog('Running ' + testName + ' against ' + coll.toString());
        var getResult = nonCursorTestCases[testName].run;
        var expectedBefore = nonCursorTestCases[testName].expectedBefore;
        var expectedAfter = nonCursorTestCases[testName].expectedAfter;

        // Setup initial state.
        assert.commandWorked(coll.remove({}));
        assert.commandWorked(coll.save({_id: 1, state: 'before', point: [0, 0]}));
        setCommittedSnapshot(makeSnapshot());

        // Check initial conditions.
        assert.eq(getResult(coll), expectedBefore);

        // Change state without making it committed.
        assert.commandWorked(coll.save({_id: 1, state: 'after', point: [0, 0]}));

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

var replTest = new ReplSetTest({
    nodes: 1,
    oplogSize: 2,
    nodeOptions: {
        setParameter: 'testingSnapshotBehaviorInIsolation=true',
        enableMajorityReadConcern: '',
        shardsvr: ''
    }
});
replTest.startSet();
// Cannot wait for a stable recovery timestamp with 'testingSnapshotBehaviorInIsolation' set.
replTest.initiateWithAnyNodeAsPrimary(
    null, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});

var mongod = replTest.getPrimary();

// Do a majority write to wait for a valid committed snapshot before starting the test. This is
// needed to make sure no oplog holes behind the clusterTime and all internal writes as part of the
// server startup are committed. Otherwise, manually setting the committed snapshot to the latest
// clusterTime using the `setCommittedSnapshot` command could result in reading ahead of the
// all_durable.
assert.commandWorked(mongod.getDB("test")["coll"].insert({x: 1}, {writeConcern: {w: "majority"}}));

(function testSingleNode() {
    var db = mongod.getDB("singleNode");
    runTests(db.collection, mongod);
})();

var shardingTest = new ShardingTest({
    shards: 0,
    mongos: 1,
});
assert(shardingTest.adminCommand({addShard: replTest.getURL()}));

(function testUnshardedDBThroughMongos() {
    var db = shardingTest.getDB("throughMongos");
    runTests(db.unshardedDB, mongod);
})();

shardingTest.adminCommand({enableSharding: 'throughMongos'});

(function testUnshardedCollectionThroughMongos() {
    var db = shardingTest.getDB("throughMongos");
    runTests(db.unshardedCollection, mongod);
})();

(function testShardedCollectionThroughMongos() {
    var db = shardingTest.getDB("throughMongos");
    var collection = db.shardedCollection;
    shardingTest.adminCommand({shardCollection: collection.getFullName(), key: {_id: 1}});
    runTests(collection, mongod);
})();

shardingTest.stop();
replTest.stopSet();
})();
