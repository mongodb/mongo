/**
 * Tests that read operations with readConcern majority only see committed data.
 *
 * The following read operations are tested:
 *  - find command
 *  - aggregation
 *  - distinct
 *  - group
 *  - count
 *  - parallelCollectionScan
 *  - geoNear
 *  - geoSearch
 *
 * Each operation is tested on a single node, and (if supported) through mongos on both sharded and
 * unsharded collections. Mongos doesn't directly handle readConcern majority, but these tests
 * should ensure that it correctly propagates the setting to the shards when running commands.
 */

(function() {
    'use strict';

    function makeCursor(mongo, result) {
        return new DBCommandCursor(mongo, result);
    }

    // These test cases are functions that return a cursor of the documents in collections without
    // fetching them yet.
    var cursorTestCases = {
        find: function(coll) {
            return makeCursor(coll.getMongo(),
                              assert.commandWorked(coll.runCommand(
                                  'find', {readConcern: {level: 'majority'}, batchSize: 0})));
        },
        aggregate: function(coll) {
            return makeCursor(
                coll.getMongo(),
                assert.commandWorked(coll.runCommand(
                    'aggregate',
                    {readConcern: {level: 'majority'}, cursor: {batchSize: 0}, pipeline: []})));
        },
        parallelCollectionScan: function(coll) {
            var res = coll.runCommand('parallelCollectionScan',
                                      {readConcern: {level: 'majority'}, numCursors: 1});
            assert.commandWorked(res);
            assert.eq(res.cursors.length, 1, tojson(res));
            return makeCursor(coll.getMongo(), res.cursors[0]);
        },
    };

    // These test cases have a run method that will be passed a collection with a single object with
    // _id: 1 and a state field that equals either "before" or "after". The collection will also
    // contain both a 2dsphere and a geoHaystack index to enable testing commands that depend on
    // them. The return value from the run method is expected to be the value of expectedBefore or
    // expectedAfter depending on the state of the state field.
    var nonCursorTestCases = {
        count_before: {
            run: function(coll) {
                var res = coll.runCommand(
                    'count', {readConcern: {level: 'majority'}, query: {state: 'before'}});
                assert.commandWorked(res);
                return res.n;
            },
            expectedBefore: 1,
            expectedAfter: 0,
        },
        count_after: {
            run: function(coll) {
                var res = coll.runCommand(
                    'count', {readConcern: {level: 'majority'}, query: {state: 'after'}});
                assert.commandWorked(res);
                return res.n;
            },
            expectedBefore: 0,
            expectedAfter: 1,
        },
        distinct: {
            run: function(coll) {
                var res =
                    coll.runCommand('distinct', {readConcern: {level: 'majority'}, key: 'state'});
                assert.commandWorked(res);
                assert.eq(res.values.length, 1, tojson(res));
                return res.values[0];
            },
            expectedBefore: 'before',
            expectedAfter: 'after',
        },
        geoNear: {
            run: function(coll) {
                var res = coll.runCommand('geoNear', {
                    readConcern: {level: 'majority'},
                    near: [0, 0],
                    spherical: true,
                });
                assert.commandWorked(res);
                assert.eq(res.results.length, 1, tojson(res));
                return res.results[0].obj.state;
            },
            expectedBefore: 'before',
            expectedAfter: 'after',
        },
        geoSearch: {
            run: function(coll) {
                var res = coll.runCommand('geoSearch', {
                    readConcern: {level: 'majority'},
                    near: [0, 0],
                    search: {_id: 1},  // Needed due to SERVER-23158.
                    maxDistance: 1,
                });
                assert.commandWorked(res);
                assert.eq(res.results.length, 1, tojson(res));
                return res.results[0].state;
            },
            expectedBefore: 'before',
            expectedAfter: 'after',
        },
        group: {
            run: function(coll) {
                var res = coll.runCommand({
                    'group': {
                        ns: coll.getName(),
                        key: {_id: 1},
                        initial: {},
                        $reduce: function(curr, result) {
                            result.state = curr.state;
                        },
                    },
                    readConcern: {level: 'majority'},
                });
                assert.commandWorked(res);
                assert.eq(res.retval.length, 1, tojson(res));
                return res.retval[0].state;
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

        for (var testName in cursorTestCases) {
            jsTestLog('Running ' + testName + ' against ' + coll.toString());
            var getCursor = cursorTestCases[testName];

            // Setup initial state.
            assert.writeOK(coll.remove({}));
            assert.writeOK(coll.save({_id: 1, state: 'before'}));
            setCommittedSnapshot(makeSnapshot());

            // Check initial conditions.
            assert.eq(getCursor(coll).next().state, 'before');

            // Change state without making it committed.
            assert.writeOK(coll.save({_id: 1, state: 'after'}));

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

        assert.commandWorked(coll.ensureIndex({point: '2dsphere'}));
        assert.commandWorked(coll.ensureIndex({point: 'geoHaystack', _id: 1}, {bucketSize: 1}));
        for (var testName in nonCursorTestCases) {
            jsTestLog('Running ' + testName + ' against ' + coll.toString());
            var getResult = nonCursorTestCases[testName].run;
            var expectedBefore = nonCursorTestCases[testName].expectedBefore;
            var expectedAfter = nonCursorTestCases[testName].expectedAfter;

            // Setup initial state.
            assert.writeOK(coll.remove({}));
            assert.writeOK(coll.save({_id: 1, state: 'before', point: [0, 0]}));
            setCommittedSnapshot(makeSnapshot());

            // Check initial conditions.
            assert.eq(getResult(coll), expectedBefore);

            // Change state without making it committed.
            assert.writeOK(coll.save({_id: 1, state: 'after', point: [0, 0]}));

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

    var mongod = MongoRunner.runMongod({setParameter: 'testingSnapshotBehaviorInIsolation=true'});
    assert.neq(
        null,
        mongod,
        'mongod was unable to start with the testingSnapshotBehaviorInIsolation parameter enabled');

    if (!mongod.adminCommand('serverStatus').storageEngine.supportsCommittedReads) {
        print("Skipping read_majority_reads.js since storageEngine doesn't support it.");
        return;
    }

    (function testSingleNode() {
        var db = mongod.getDB("singleNode");
        runTests(db.collection, mongod);
    })();

    var shardingTest = new ShardingTest({
        shards: 0,  // We use the existing mongod.
        mongos: 1,
    });
    shardingTest.adminCommand({addShard: mongod.host});

    // Remove tests of commands that aren't supported at all through mongos, even on unsharded
    // collections.
    ['parallelCollectionScan', 'geoSearch'].forEach(function(cmd) {
        // Make sure it really isn't supported.
        assert.eq(shardingTest.getDB('test').coll.runCommand(cmd).code, ErrorCodes.CommandNotFound);
        delete cursorTestCases[cmd];
        delete nonCursorTestCases[cmd];
    });

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

        // The group command isn't supported on sharded collections. It also uses a weird syntax so
        // code to check that it isn't supported can't be reused with other commands.
        assert.eq(collection.runCommand({'group': {ns: collection.getName()}}).code,
                  ErrorCodes.IllegalOperation);
        delete nonCursorTestCases.group;

        runTests(collection, mongod);
    })();

    shardingTest.stop();
    MongoRunner.stopMongod(mongod);
})();
