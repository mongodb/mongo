/**
 * Tests that the coordinateCommitTransaction command falls back to recovering the decision from
 * the local participant.
 *
 * TODO (SERVER-37364): Once coordinateCommit returns as soon as the decision is made durable, these
 * tests will pass but will be racy in terms of whether they're testing that coordinateCommit
 * returns the TransactionCoordinator's decision or local TransactionParticipant's decision.
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
    TestData.transactionLifetimeLimitSeconds = 15;

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
        assert.neq(null, res.recoveryToken.shardId);
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
        assert.neq(null, res.recoveryToken.shardId);
        return res.recoveryToken;
    };

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

    const lsid = {id: UUID()};
    let txnNumber = 0;
    const participantList = [{shardId: st.shard0.shardName}, {shardId: st.shard1.shardName}];

    (function() {
        jsTest.log("coordinator's participant has never heard of the session.");
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, participantList),
                                     ErrorCodes.NoSuchTransaction);
    })();

    (function() {
        jsTest.log("coordinator finished coordinating an abort decision.");

        txnNumber++;

        const recoveryToken = startNewCrossShardTransactionThroughMongos();
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
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, []),
                                     ErrorCodes.NoSuchTransaction);

        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.NoSuchTransaction);

    })();

    (function() {
        jsTest.log("coordinator finished coordinating a commit decision.");

        txnNumber++;

        const recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));

        assert.commandWorked(runCoordinateCommit(txnNumber, participantList));
        assert.commandWorked(runCoordinateCommit(txnNumber, []));

        assert.commandWorked(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken));
    })();

    (function() {
        jsTest.log(
            "coordinator finished coordinating a commit decision but coordinator node can't majority commit writes");

        txnNumber++;

        const recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"}
        }));

        stopReplicationOnSecondaries(coordinatorReplSetTest);

        // Do a write on the coordinator to bump the coordinator node's system last OpTime.
        coordinatorPrimaryConn.getDB("dummy").getCollection("dummy").insert({dummy: 1});

        // While the coordinator primary cannot majority commit writes, coordinateCommitTransaction
        // returns ok:1 with a writeConcern error.

        let res = runCoordinateCommit(
            txnNumber, participantList, {writeConcern: {w: "majority", wtimeout: 500}});
        assert.eq(1, res.ok, tojson(res));
        checkWriteConcernTimedOut(res);

        res = runCoordinateCommit(txnNumber, [], {writeConcern: {w: "majority", wtimeout: 500}});
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
        assert.commandWorked(runCoordinateCommit(txnNumber, [], {writeConcern: {w: "majority"}}));
        assert.commandWorked(sendCommitViaOtherMongos(
            lsid, txnNumber, recoveryToken, {writeConcern: {w: "majority"}}));
    })();

    (function() {
        jsTest.log(
            "try to recover decision for lower transaction number than last number participant saw.");

        ++txnNumber;

        const oldTxnNumber = txnNumber;
        const recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
        }));

        txnNumber++;

        startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
        }));

        assert.commandFailedWithCode(runCoordinateCommit(oldTxnNumber, participantList),
                                     ErrorCodes.TransactionTooOld);
        assert.commandFailedWithCode(runCoordinateCommit(oldTxnNumber, []),
                                     ErrorCodes.TransactionTooOld);
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, oldTxnNumber, recoveryToken),
                                     ErrorCodes.TransactionTooOld);

        // Can still recover decision for current transaction number.
        assert.commandWorked(runCoordinateCommit(txnNumber, participantList));
        assert.commandWorked(runCoordinateCommit(txnNumber, []));
        assert.commandWorked(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken));
    })();

    (function() {
        jsTest.log("session has been reaped.");

        txnNumber++;

        const recoveryToken = startNewCrossShardTransactionThroughMongos();
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"}
        }));

        assert.writeOK(
            coordinatorPrimaryConn.getDB("config").transactions.remove({}, false /* justOne */));

        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, participantList),
                                     ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, []),
                                     ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.NoSuchTransaction);
    })();

    (function() {
        jsTest.log(
            "try to recover decision for higher transaction number than participant has seen.");

        txnNumber++;
        const recoveryToken = startNewCrossShardTransactionThroughMongos();

        txnNumber++;
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, participantList),
                                     ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(runCoordinateCommit(txnNumber, []),
                                     ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.NoSuchTransaction);

        // We can still commit the active transaction with the lower transaction number.
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber - 1),
            autocommit: false
        }));
    })();

    (function() {
        jsTest.log("commit was never sent");

        txnNumber++;

        // Start transaction and run CRUD ops on several shards.
        const recoveryToken = startNewCrossShardTransactionThroughMongos();

        // Try to recover decision from other mongos. This should block until the transaction
        // coordinator is canceled after transactionLifetimeLimitSeconds, after which it should
        // abort the local participant and return NoSuchTransaction.
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.NoSuchTransaction);
    })();

    (function() {
        jsTest.log(
            "try to recover decision for single-shard transaction that has already committed");

        txnNumber++;

        const recoveryToken = startNewSingleShardTransactionThroughMongos();

        // Commit the transaction from the first mongos.
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            recoveryToken: recoveryToken
        }));

        // Try to recover decision from other mongos.
        assert.commandWorked(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken));
    }());

    (function() {
        jsTest.log("try to recover decision for single-shard transaction that has already aborted");

        txnNumber++;

        const recoveryToken = startNewSingleShardTransactionThroughMongos();
        assert.commandWorked(st.rs0.getPrimary().adminCommand({
            abortTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));

        // Commit the transaction from the first mongos.
        assert.commandFailedWithCode(testDB.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            recoveryToken: recoveryToken
        }),
                                     ErrorCodes.NoSuchTransaction);

        // Try to recover the decision from other mongos.
        assert.commandFailedWithCode(sendCommitViaOtherMongos(lsid, txnNumber, recoveryToken),
                                     ErrorCodes.NoSuchTransaction);
    }());

    st.stop();
})();
