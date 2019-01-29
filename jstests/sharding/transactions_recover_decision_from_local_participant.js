/**
 * Tests that the coordinateCommitTransaction command falls back to recovering the decision from
 * the local participant.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");

    // The test modifies config.transactions, which must be done outside of a session.
    TestData.disableImplicitSessions = true;
    // Reducing this from the resmoke default, which is several hours, so that tests that rely on a
    // transaction coordinator being canceled after a timeout happen in a reasonable amount of time.
    TestData.transactionLifetimeLimitSeconds = 60;

    let st =
        new ShardingTest({shards: 2, rs: {nodes: 2}, mongos: 2, other: {rsOptions: {verbose: 2}}});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.name);
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: 'test.user', middle: {x: 0}}));
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.name}));

    // Insert documents to prime mongos and shards with the latest sharding metadata.
    let testDB = st.s0.getDB('test');
    assert.commandWorked(testDB.runCommand({insert: 'user', documents: [{x: -10}, {x: 10}]}));

    let coordinatorReplSetTest = st.rs0;
    let coordinatorPrimaryConn = coordinatorReplSetTest.getPrimary();

    const runCoordinateCommit = function(txnNumber, participantList, writeConcern) {
        writeConcern = writeConcern || {};
        return coordinatorPrimaryConn.adminCommand(Object.merge({
            coordinateCommitTransaction: 1,
            participants: participantList,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        },
                                                                writeConcern));
    };

    const startNewSingleShardTransactionThroughMongos = function() {
        const updateDocumentOnShard0 = {
            q: {x: -1},
            u: {"$set": {lastTxnNumber: txnNumber}},
            upsert: true
        };

        let res = assert.commandWorked(testDB.runCommand({
            update: 'user',
            updates: [updateDocumentOnShard0],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true
        }));

        assert.neq(null, res.recoveryToken);
        return res.recoveryToken;
    };

    const startNewCrossShardTransactionThroughMongos = function() {
        const updateDocumentOnShard0 = {
            q: {x: -1},
            u: {"$set": {lastTxnNumber: txnNumber}},
            upsert: true
        };
        const updateDocumentOnShard1 = {
            q: {x: 1},
            u: {"$set": {lastTxnNumber: txnNumber}},
            upsert: true
        };

        let res = assert.commandWorked(testDB.runCommand({
            update: 'user',
            updates: [updateDocumentOnShard0, updateDocumentOnShard1],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            startTransaction: true
        }));

        assert.neq(null, res.recoveryToken);
        return res.recoveryToken;
    };

    const sendCommitViaOtherMongos = function(lsid, txnNumber, recoveryToken, writeConcern) {
        writeConcern = writeConcern || {};
        return st.s1.getDB('admin').runCommand(Object.merge({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            recoveryToken: recoveryToken
        },
                                                            writeConcern));
    };

    // TODO (SERVER-37364): Once coordinateCommit returns as soon as the decision is made durable,
    // this test will pass but will be racy in terms of whether it's testing that coordinateCommit
    // returns the TransactionCoordinator's decision or local TransactionParticipant's decision.
    const runTest = function(participantList) {
        jsTest.log("running test with participant list: " + tojson(participantList));

        jsTest.log("coordinateCommit sent when local participant has never heard of the session.");
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, participantList),
                                     ErrorCodes.NoSuchTransaction);

        jsTest.log(
            "coordinateCommit sent after coordinator finished coordinating an abort decision.");
        ++txnNumber;

        let recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(st.rs0.getPrimary().adminCommand({
            abortTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));
        assert.commandFailedWithCode(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }),
                                     ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, participantList),
                                     ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.NoSuchTransaction);

        jsTest.log(
            "coordinateCommit sent after coordinator finished coordinating a commit decision.");
        ++txnNumber;

        recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));
        assert.commandWorked(runCoordinateCommit(txnNumber, participantList));
        assert.commandWorked(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken));

        jsTest.log(
            "coordinateCommit sent after coordinator finished coordinating a commit decision but coordinator node can't majority commit writes");
        ++txnNumber;

        recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"}
        }));

        // While the coordinator primary cannot majority commit writes, coordinateCommitTransaction
        // returns ok:1 with a writeConcern error.

        stopReplicationOnSecondaries(coordinatorReplSetTest);

        // Do a write on the coordinator to bump the coordinator node's system last OpTime.
        coordinatorPrimaryConn.getDB("dummy").getCollection("dummy").insert({dummy: 1});

        let res = runCoordinateCommit(
            txnNumber, participantList, {writeConcern: {w: "majority", wtimeout: 500}});
        assert.eq(1, res.ok, tojson(res));
        checkWriteConcernTimedOut(res);

        res = sendCommitViaOtherMongos(
            lsid, txnNumber, recoveryToken, {writeConcern: {w: "majority", wtimeout: 500}});
        assert.eq(1, res.ok, tojson(res));
        checkWriteConcernTimedOut(res);

        // Once the coordinator primary can majority commit writes again,
        // coordinateCommitTransaction returns ok:1 without a writeConcern error.
        restartReplicationOnSecondaries(coordinatorReplSetTest);
        assert.commandWorked(
            runCoordinateCommit(txnNumber, participantList, {writeConcern: {w: "majority"}}));
        assert.commandWorked(sendCommitViaOtherMongos(
            lsid, txnNumber, recoveryToken, {writeConcern: {w: "majority"}}));

        jsTest.log(
            "coordinateCommit sent for lower transaction number than last number participant saw.");
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber - 1, participantList),
                                     ErrorCodes.TransactionTooOld);

        // Can still recover decision for current transaction number.
        assert.commandWorked(runCoordinateCommit(txnNumber, participantList));

        jsTest.log("coordinateCommit sent after session is reaped.");
        assert.writeOK(
            coordinatorPrimaryConn.getDB("config").transactions.remove({}, false /* justOne */));
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, participantList),
                                     ErrorCodes.NoSuchTransaction);

        jsTest.log(
            "coordinateCommit sent for higher transaction number than participant has seen.");
        ++txnNumber;
        recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber + 1, participantList),
                                     ErrorCodes.NoSuchTransaction);

        // We can still commit the active transaction with the lower transaction number.
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));
        assert.commandWorked(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken));
    };

    // Test with a real participant list, to simulate retrying through main router.
    let lsid = {id: UUID()};
    let txnNumber = 0;
    runTest([{shardId: st.shard0.shardName}, {shardId: st.shard1.shardName}]);

    // Test with an empty participant list, to simulate using a recovery router.
    lsid = {id: UUID()};
    txnNumber = 0;
    runTest([]);

    /**
     * Test that commit recovery succeeds with a single-shard transaction that has already
     * committed.
     */
    (function() {
        lsid = {id: UUID()};
        txnNumber = 0;

        // Start single-shard transaction so that coordinateCommit is not necessary for commit.
        let recoveryToken = startNewSingleShardTransactionThroughMongos();

        // Commit the transaction from the first mongos.
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            recoveryToken: recoveryToken
        }));

        // Try to recover decision from other mongos. This should block until the coordinator is
        // removed and then return the commit decision (which was commit).
        assert.commandWorked(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken));
    }());

    /**
     * Test that commit recovery succeeds with a multi-shard transaction for which commit is never
     * sent.
     */
    (function() {
        lsid = {id: UUID()};
        txnNumber = 0;

        // Start transaction and run CRUD ops on several shards.
        let recoveryToken = startNewCrossShardTransactionThroughMongos();

        // Try to recover decision from other mongos. This should block until the transaction
        // coordinator is canceled after transactionLifetimeLimitSeconds, after which it should
        // abort the local participant and return NoSuchTransaction.
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.NoSuchTransaction);
    })();

    st.stop();
})();
