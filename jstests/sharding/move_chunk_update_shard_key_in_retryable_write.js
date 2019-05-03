/**
 * When we update a shard key such that a document moves from one shard to another, and the
 * update is sent as a retryable write, the update is converted into a multi-statement
 * transaction with the same transaction number. The new transaction contains a delete on the source
 * shard and an insert on the destination shard. If original update is retried, it should receive an
 * error saying that the write can't be retried since it was upgraded to a transaction as part of
 * the update. This should be true whether or not a migration occurs on the chunk containing the
 * original value of the document's shard key. This file tests that behavior.
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */
(function() {

    "use strict";

    load("jstests/libs/retryable_writes_util.js");
    load('jstests/sharding/libs/sharded_transactions_helpers.js');
    load('./jstests/libs/chunk_manipulation_util.js');

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    // For startParallelOps to write its state
    let staticMongod = MongoRunner.runMongod({});

    let st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 2}, rs2: {nodes: 2}}});

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;
    let testDB = st.s.getDB(dbName);
    let testColl = testDB.foo;

    // Create a sharded collection with three chunks:
    //     [-inf, -10), [-10, 10), [10, inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -10}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));

    /**
     * Sets up a test by moving chunks to such that one chunk is on each
     * shard, with the following distribution:
     *     shard0: [-inf, -10)
     *     shard1: [-10, 10)
     *     shard2: [10, inf)
     */
    function setUp() {
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard0.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {x: 1000}, to: st.shard2.shardName}));

        flushRoutersAndRefreshShardMetadata(st, {ns});
    }

    /**
     * Tears down a test by dropping all documents from the test collection.
     */
    function tearDown() {
        assert.commandWorked(testColl.deleteMany({}));
    }

    /**
     * Generic function to run a test. 'description' is a description of the test for logging
     * purposes and 'testBody' is the test function.
     */
    function test(description, testBody) {
        jsTest.log(`Running Test Setup: ${description}`);
        setUp();
        jsTest.log(`Running Test Body: ${description}`);
        testBody();
        jsTest.log(`Running Test Tear-Down: ${description}`);
        tearDown();
        jsTest.log(`Finished Running Test: ${description}`);
    }

    test("Updating shard key in retryable write receives error on retry", () => {
        const shardKeyValueOnShard0 = -100;
        const shardKeyValueOnShard1 = 0;

        // Insert a single document on shard 0.
        testColl.insert({x: shardKeyValueOnShard0});

        const cmdObj = {
            update: collName,
            updates: [
                {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
            ],
            ordered: false,
            lsid: {id: UUID()},
            txnNumber: NumberLong(35),
        };

        // Update the document shard key. The document should now be on shard 1.
        const result = assert.commandWorked(testDB.runCommand(cmdObj));
        assert.eq(result.n, 1);
        assert.eq(result.nModified, 1);
        assert.eq(testColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

        // Retry the command. This should retry against shard 0, which should throw
        // IncompleteTransactionHistory.
        assert.commandFailedWithCode(testDB.runCommand(cmdObj),
                                     ErrorCodes.IncompleteTransactionHistory);
    });

    test(
        "Updating shard key in retryable write receives error on retry when the original chunk has been migrated to a new shard",
        () => {
            const shardKeyValueOnShard0 = -100;
            const shardKeyValueOnShard1 = 0;

            // Insert a single document on shard 0.
            testColl.insert({x: shardKeyValueOnShard0});

            const cmdObj = {
                update: collName,
                updates: [
                    {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
                ],
                ordered: false,
                lsid: {id: UUID()},
                txnNumber: NumberLong(35),
            };

            // Update the document shard key. The document should now be on shard 1.
            const result = assert.commandWorked(testDB.runCommand(cmdObj));
            assert.eq(result.n, 1);
            assert.eq(result.nModified, 1);
            assert.eq(testColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

            // Move the chunk that contained the original document to shard 1.
            assert.commandWorked(st.s.adminCommand(
                {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard1.shardName}));

            // Retry the command. This should retry against shard 1, which should throw
            // IncompleteTransactionHistory.
            assert.commandFailedWithCode(testDB.runCommand(cmdObj),
                                         ErrorCodes.IncompleteTransactionHistory);

        });

    test(
        "Updating shard key in retryable write receives error on retry when the original chunk has been migrated to a new shard and then to a third shard",
        () => {
            const shardKeyValueOnShard0 = -100;
            const shardKeyValueOnShard1 = 0;

            // Insert a single document on shard 0.
            testColl.insert({x: shardKeyValueOnShard0});

            const cmdObj = {
                update: collName,
                updates: [
                    {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
                ],
                ordered: false,
                lsid: {id: UUID()},
                txnNumber: NumberLong(35),
            };

            // Update the document shard key. The document should now be on shard 1.
            const result = assert.commandWorked(testDB.runCommand(cmdObj));
            assert.eq(result.n, 1);
            assert.eq(result.nModified, 1);
            assert.eq(testColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

            // Move the chunk that contained the original document to shard 1.
            assert.commandWorked(st.s.adminCommand(
                {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard1.shardName}));

            // Then move the same chunk that contained the original document to shard 2.
            assert.commandWorked(st.s.adminCommand(
                {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard2.shardName}));

            // Retry the command. This should retry against shard 1, which should throw
            // IncompleteTransactionHistory.
            assert.commandFailedWithCode(testDB.runCommand(cmdObj),
                                         ErrorCodes.IncompleteTransactionHistory);
        });

    test(
        "Updating shard key in retryable write receives error on retry when the original chunk has been migrated to a shard without knowledge of the transaction",
        () => {
            const shardKeyValueOnShard0 = -100;
            const shardKeyValueOnShard1 = 0;

            // Insert a single document on shard 0.
            testColl.insert({x: shardKeyValueOnShard0});

            const cmdObj = {
                update: collName,
                updates: [
                    {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
                ],
                ordered: false,
                lsid: {id: UUID()},
                txnNumber: NumberLong(35),
            };

            // Update the document shard key. The document should now be on shard 1.
            const result = assert.commandWorked(testDB.runCommand(cmdObj));
            assert.eq(result.n, 1);
            assert.eq(result.nModified, 1);
            assert.eq(testColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

            // Move the chunk that contained the original document to shard 2,
            // which does not know about the tranasaction.
            assert.commandWorked(st.s.adminCommand(
                {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard2.shardName}));

            // Retry the command. This should retry against shard 2, which should throw
            // IncompleteTransactionHistory.
            assert.commandFailedWithCode(testDB.runCommand(cmdObj),
                                         ErrorCodes.IncompleteTransactionHistory);
        });

    test(
        "config.transactions entries for single-shard transactions which commit during transferMods phase are successfully migrated as dead-end sentinels",
        () => {
            const shardKeyValueOnShard0 = -100;
            const anotherShardKeyValueOnShard0 = -101;
            const shardKeyValueOnShard1 = 0;
            const lsid = {id: UUID()};
            const txnNumber = 35;

            // Insert a single document on shard 0.
            assert.commandWorked(testColl.insert({x: shardKeyValueOnShard0}));

            const cmdToRunInTransaction = {
                update: collName,
                updates: [
                    // Add a new field.
                    {q: {x: shardKeyValueOnShard0}, u: {$set: {a: 4}}},
                ],
                ordered: false,
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                startTransaction: true,
                autocommit: false
            };

            const fakeRetryCmd = {
                update: collName,
                updates: [
                    // Add a new field.
                    {q: {x: shardKeyValueOnShard0}, u: {$set: {a: 4}}},
                ],
                ordered: false,
                lsid: lsid,
                txnNumber: NumberLong(txnNumber)
            };

            pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

            let joinMoveChunk = moveChunkParallel(
                staticMongod, st.s.host, {x: shardKeyValueOnShard0}, null, ns, st.shard1.shardName);

            waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

            // Update a document being migrated.
            const result = assert.commandWorked(testDB.runCommand(cmdToRunInTransaction));
            assert.eq(result.n, 1);
            assert.eq(result.nModified, 1);

            assert.commandWorked(testDB.adminCommand({
                commitTransaction: 1,
                writeConcern: {w: "majority"},
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                autocommit: false
            }));

            // Check that the update from the transaction succeeded.
            const resultingDoc = testColl.findOne({x: shardKeyValueOnShard0});
            assert.neq(resultingDoc, null);
            assert.eq(resultingDoc["a"], 4);

            unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

            // Wait for moveChunk to complete
            joinMoveChunk();

            st.printShardingStatus();
            // Retry the command. This should retry against shard 1, which should throw
            // IncompleteTransactionHistory.
            assert.commandFailedWithCode(testDB.runCommand(fakeRetryCmd),
                                         ErrorCodes.IncompleteTransactionHistory);
        });

    test(
        "Update to shard key in retryable write during transferMods phase of chunk migration is migrated successfully to a node not involved in the shard key update",
        () => {
            const shardKeyValueOnShard0 = -100;
            const shardKeyValueOnShard1 = 0;
            const docId = 0;

            // Insert a single document on shard 0.
            assert.commandWorked(testColl.insert({_id: docId, x: shardKeyValueOnShard0}));

            const cmdObj = {
                update: collName,
                updates: [
                    {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
                ],
                ordered: false,
                lsid: {id: UUID()},
                txnNumber: NumberLong(35),
            };

            pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

            // We're going to do a shard key update to move a document from shard 0 to shard 1, so
            // here we move the chunk from shard 0 to shard 2, which won't be involved in the
            // transaction created by the shard key update.
            let joinMoveChunk = moveChunkParallel(
                staticMongod, st.s.host, {x: shardKeyValueOnShard0}, null, ns, st.shard2.shardName);

            waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

            // Update the document shard key so that the document will move from shard 0 to shard 1.
            const result = assert.commandWorked(testDB.runCommand(cmdObj));
            assert.eq(result.n, 1);
            assert.eq(result.nModified, 1);
            assert.eq(testColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

            unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

            // Wait for moveChunk to complete
            joinMoveChunk();

            st.printShardingStatus();
            // Retry the command. This should retry against shard 2, which should throw
            // IncompleteTransactionHistory.
            assert.commandFailedWithCode(testDB.runCommand(cmdObj),
                                         ErrorCodes.IncompleteTransactionHistory);
        });

    // TODO (SERVER-40815) This test currently fails with DuplicateKeyError on _id.
    //
    // test(
    //    "Update to shard key in retryable write during transfer mods phase of chunk migration is
    //    migrated successfully ",
    //    () => {
    //        const shardKeyValueOnShard0 = -100;
    //        const shardKeyValueOnShard1 = 0;
    //        const docId = 0;

    //        // Insert a single document on shard 0.
    //        assert.commandWorked(testColl.insert({_id: docId, x: shardKeyValueOnShard0}));

    //        const cmdObj = {
    //            update: collName,
    //            updates: [
    //                {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
    //            ],
    //            ordered: false,
    //            lsid: {id: UUID()},
    //            txnNumber: NumberLong(35),
    //        };

    //        pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

    //        let joinMoveChunk = moveChunkParallel(
    //            staticMongod, st.s.host, {x: shardKeyValueOnShard0}, null, ns,
    //            st.shard1.shardName);

    //        waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

    //        // Update the document shard key.

    //        // THIS CURRENTLY FAILS WITH DuplicateKeyError on _id
    //        const result = assert.commandWorked(testDB.runCommand(cmdObj));
    //        assert.eq(result.n, 1);
    //        assert.eq(result.nModified, 1);
    //        assert.eq(testColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

    //        unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

    //        // Wait for moveChunk to complete
    //        joinMoveChunk();

    //        st.printShardingStatus();
    //        // Retry the command. This should retry against shard 1, which should throw
    //        // IncompleteTransactionHistory.
    //        assert.commandFailedWithCode(testDB.runCommand(cmdObj),
    //                                     ErrorCodes.IncompleteTransactionHistory);
    //    });

    st.stop();
    MongoRunner.stopMongod(staticMongod);
})();
