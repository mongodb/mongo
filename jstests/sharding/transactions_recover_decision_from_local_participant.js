/**
 * Tests that the coordinateCommitTransaction command falls back to recovering the decision from
 * the local participant.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    // The test modifies config.transactions, which must be done outside of a session.
    TestData.disableImplicitSessions = true;

    let st = new ShardingTest({shards: 2, mongos: 2});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.name);
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: 'test.user', middle: {x: 0}}));
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.name}));

    // Insert documents to prime mongos and shards with the latest sharding metadata.
    let testDB = st.s0.getDB('test');
    assert.commandWorked(testDB.runCommand({insert: 'user', documents: [{x: -10}, {x: 10}]}));

    let coordinatorConn = st.rs0.getPrimary();

    const runCoordinateCommit = function(txnNumber, participantList) {
        return coordinatorConn.adminCommand({
            coordinateCommitTransaction: 1,
            participants: participantList,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        });
    };

    const startNewTransactionThroughMongos = function() {
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

    const sendCommitViaOtherMongos = function(lsid, txnNumber, recoveryToken) {
        return st.s1.getDB('admin').runCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            recoveryToken: recoveryToken
        });
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

        let recoveryToken = startNewTransactionThroughMongos();
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

        recoveryToken = startNewTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));
        assert.commandWorked(runCoordinateCommit(txnNumber, participantList));

        assert.commandWorked(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken));

        jsTest.log(
            "coordinateCommit sent for lower transaction number than last number participant saw.");
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber - 1, participantList),
                                     ErrorCodes.TransactionTooOld);

        // Can still recover decision for current transaction number.
        assert.commandWorked(runCoordinateCommit(txnNumber, participantList));

        jsTest.log("coordinateCommit sent after session is reaped.");
        assert.writeOK(
            coordinatorConn.getDB("config").transactions.remove({}, false /* justOne */));
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, participantList),
                                     ErrorCodes.NoSuchTransaction);

        jsTest.log(
            "coordinateCommit sent for higher transaction number than participant has seen.");
        ++txnNumber;
        recoveryToken = startNewTransactionThroughMongos();
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber + 1, participantList),
                                     ErrorCodes.NoSuchTransaction);

        // Expedite aborting the transaction on the non-coordinator shard.
        // The previous transaction should abort because sending coordinateCommit with a higher
        // transaction number against the coordinator should have caused the local participant on
        // the coordinator to abort.
        assert.commandFailedWithCode(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }),
                                     ErrorCodes.NoSuchTransaction);

        // Previous commit already discarded the coordinator since it aborted, so we get
        // "transaction too old" instead.
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.TransactionTooOld);
    };

    // Test with a real participant list, to simulate retrying through main router.
    let lsid = {id: UUID()};
    let txnNumber = 0;
    runTest([{shardId: st.shard0.shardName}, {shardId: st.shard1.shardName}]);

    // Test with an empty participant list, to simulate using a recovery router.
    lsid = {id: UUID()};
    txnNumber = 0;
    runTest([]);

    st.stop();
})();
