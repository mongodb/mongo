// Tests snapshot isolation on readConcern level snapshot reads through mongos.
// @tags: [requires_sharding, uses_transactions, uses_multi_shard_transaction]
(function() {
    "use strict";

    // This test intentionally runs commands without a logical session id, which is not compatible
    // with implicit sessions.
    TestData.disableImplicitSessions = true;

    load("jstests/libs/global_snapshot_reads_util.js");
    load("jstests/sharding/libs/sharded_transactions_helpers.js");

    const dbName = "test";
    const shardedCollName = "shardedColl";
    const unshardedCollName = "unshardedColl";

    const commands = {
        aggregate: {
            firstCommand: function(collName) {
                return {
                    aggregate: collName,
                    pipeline: [{$sort: {_id: 1}}],
                    cursor: {batchSize: 5},
                    readConcern: {level: "snapshot"},
                };
            },
            secondCommand: function(collName) {
                return {
                    aggregate: collName,
                    pipeline: [{$sort: {_id: 1}}],
                    cursor: {batchSize: 20},
                    readConcern: {level: "snapshot"},
                };
            }
        },
        find: {
            firstCommand: function(collName) {
                return {
                    find: collName,
                    sort: {_id: 1},
                    batchSize: 5,
                    readConcern: {level: "snapshot"},
                };
            },
            secondCommand: function(collName) {
                return {
                    find: collName,
                    sort: {_id: 1},
                    batchSize: 20,
                    readConcern: {level: "snapshot"},
                };
            }
        }
    };

    let shardingScenarios = {
        // Tests a snapshot cursor command in a single shard environment. The set up inserts a
        // collection, shards it if it's a collection meant to be sharded, and inserts ten
        // documents.
        singleShard: {
            compatibleCollections: [shardedCollName, unshardedCollName],
            name: "singleShard",
            setUp: function(collName) {
                const st = new ShardingTest({shards: 1, mongos: 1, config: 1});
                return shardingScenarios.allScenarios.setUp(st, collName);
            }
        },
        // Tests a snapshot cursor command in a multi shard enviroment. The set up inserts a
        // collection, shards the collection, and inserts ten documents. Afterwards, chunks are
        // split and moved such that every shard should have some documents that will be found
        // by the cursor command.
        multiShardAllShardReads: {
            compatibleCollections: [shardedCollName],
            name: "multiShardAllShardReads",
            setUp: function(collName) {
                let st = new ShardingTest({shards: 3, mongos: 1, config: 1});
                st = shardingScenarios.allScenarios.setUp(st, collName);

                if (st === undefined) {
                    return;
                }

                const mongos = st.s0;

                const ns = dbName + '.' + shardedCollName;

                assert.commandWorked(st.splitAt(ns, {_id: 4}));
                assert.commandWorked(st.splitAt(ns, {_id: 7}));

                assert.commandWorked(
                    mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));
                assert.commandWorked(
                    mongos.adminCommand({moveChunk: ns, find: {_id: 4}, to: st.shard1.shardName}));
                assert.commandWorked(
                    mongos.adminCommand({moveChunk: ns, find: {_id: 7}, to: st.shard2.shardName}));

                assert.eq(
                    1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard0.shardName}));
                assert.eq(
                    1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard1.shardName}));
                assert.eq(
                    1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard2.shardName}));

                flushRoutersAndRefreshShardMetadata(st, {ns});

                return st;
            }
        },
        // Tests a snapshot cursor command in a multi shard enviroment. The set up inserts a
        // collection, shards the collection, and inserts ten documents. Afterwards, chunks are
        // split and moved such that only two out of three shards will have documents that will be
        // found by the cursor command.
        multiShardSomeShardReads: {
            compatibleCollections: [shardedCollName],
            name: "multiShardSomeShardReads",
            setUp: function(collName) {
                let st = new ShardingTest({shards: 3, mongos: 1, config: 1});
                st = shardingScenarios.allScenarios.setUp(st, collName);

                if (st === undefined) {
                    return;
                }

                const mongos = st.s0;

                const ns = dbName + '.' + shardedCollName;

                assert.commandWorked(st.splitAt(ns, {_id: 5}));
                assert.commandWorked(
                    mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
                assert.commandWorked(
                    mongos.adminCommand({moveChunk: ns, find: {_id: 7}, to: st.shard2.shardName}));

                assert.eq(
                    0, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard0.shardName}));
                assert.eq(
                    1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard1.shardName}));
                assert.eq(
                    1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard2.shardName}));

                flushRoutersAndRefreshShardMetadata(st, {ns});

                return st;
            }
        },
        allScenarios: {
            name: "allScenarios",
            setUp: function(st, collName) {
                assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
                assert.commandWorked(st.s.adminCommand(
                    {shardCollection: st.s.getDB(dbName)[shardedCollName] + "", key: {_id: 1}}));

                const mainDb = st.s.getDB(dbName);

                let bulk = mainDb[collName].initializeUnorderedBulkOp();
                for (let x = 0; x < 10; ++x) {
                    bulk.insert({_id: x});
                }
                assert.commandWorked(bulk.execute({w: "majority"}));

                return st;
            }
        }
    };

    function runScenario(testScenario, {useCausalConsistency}) {
        testScenario.compatibleCollections.forEach(function(collName) {
            jsTestLog("Running the " + testScenario.name + " scenario on collection " + collName);
            runTest(testScenario, {useCausalConsistency, commands, collName});
        });
    }

    function runTest(testScenario, {useCausalConsistency, commands, collName}) {
        let st = testScenario.setUp(collName);
        assert(st);
        assert(commands);
        assert(collName);

        const mainDb = st.s.getDB(dbName);

        for (let commandKey in commands) {
            assert(commandKey);
            jsTestLog("Testing the " + commandKey + " command.");
            const command = commands[commandKey];

            const session =
                mainDb.getMongo().startSession({causalConsistency: useCausalConsistency});
            const lsid = session.getSessionId();
            const sessionDb = session.getDatabase(dbName);

            // Test snapshot reads.
            session.startTransaction({writeConcern: {w: "majority"}});

            let txnNumber = session.getTxnNumber_forTesting();

            // Establish a snapshot cursor, fetching the first 5 documents.
            let res = assert.commandWorked(sessionDb.runCommand(command.firstCommand(collName)));

            assert(res.hasOwnProperty("cursor"));
            assert(res.cursor.hasOwnProperty("firstBatch"));
            assert.eq(5, res.cursor.firstBatch.length);

            assert(res.cursor.hasOwnProperty("id"));
            const cursorId = res.cursor.id;
            assert.neq(cursorId, 0);

            // Insert an 11th document which should not be visible to the snapshot cursor. This
            // write is performed outside of the session.
            assert.writeOK(mainDb[collName].insert({_id: 10}, {writeConcern: {w: "majority"}}));

            verifyInvalidGetMoreAttempts(mainDb, collName, cursorId, lsid, txnNumber);

            // Fetch the 6th document. This confirms that the transaction stash is preserved across
            // multiple getMore invocations.
            res = assert.commandWorked(sessionDb.runCommand({
                getMore: cursorId,
                collection: collName,
                batchSize: 1,
            }));
            assert(res.hasOwnProperty("cursor"));
            assert(res.cursor.hasOwnProperty("id"));
            assert.neq(0, res.cursor.id);

            // Exhaust the cursor, retrieving the remainder of the result set.
            res = assert.commandWorked(sessionDb.runCommand({
                getMore: cursorId,
                collection: collName,
                batchSize: 10,
            }));

            // The cursor has been exhausted.
            assert(res.hasOwnProperty("cursor"));
            assert(res.cursor.hasOwnProperty("id"));
            assert.eq(0, res.cursor.id);

            // Only the remaining 4 of the initial 10 documents are returned. The 11th document is
            // not part of the result set.
            assert(res.cursor.hasOwnProperty("nextBatch"));
            assert.eq(4, res.cursor.nextBatch.length);

            session.commitTransaction();

            // Perform a second snapshot read under a new transaction.
            session.startTransaction({writeConcern: {w: "majority"}});
            res = assert.commandWorked(sessionDb.runCommand(command.secondCommand(collName)));

            // The cursor has been exhausted.
            assert(res.hasOwnProperty("cursor"));
            assert(res.cursor.hasOwnProperty("id"));
            assert.eq(0, res.cursor.id);

            // All 11 documents are returned.
            assert(res.cursor.hasOwnProperty("firstBatch"));
            assert.eq(11, res.cursor.firstBatch.length);

            // Remove the 11th document to preserve the collection for the next command.
            assert.writeOK(mainDb[collName].remove({_id: 10}, {writeConcern: {w: "majority"}}));

            session.commitTransaction();
            session.endSession();
        }

        st.stop();
    }

    // Runs against a sharded and unsharded collection.
    runScenario(shardingScenarios.singleShard, {useCausalConsistency: false});

    runScenario(shardingScenarios.multiShardAllShardReads, {useCausalConsistency: false});

    runScenario(shardingScenarios.multiShardSomeShardReads,
                {useCausalConsistency: false, collName: shardedCollName});
})();
