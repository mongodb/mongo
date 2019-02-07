/**
 * Exercises the coordinator commands logic by simulating a basic two phase commit and basic two
 * phase abort.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction]
 */

(function() {
    'use strict';

    load('jstests/sharding/libs/sharded_transactions_helpers.js');

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    let st = new ShardingTest({shards: 3, causallyConsistent: true});

    let coordinator = st.shard0;
    let participant1 = st.shard1;
    let participant2 = st.shard2;

    let expectedParticipantList =
        [participant1.shardName, participant2.shardName, coordinator.shardName];

    let lsid = {id: UUID()};
    let txnNumber = 0;

    const checkParticipantListMatches = function(
        coordinatorConn, lsid, txnNumber, expectedParticipantList) {
        let coordDoc = coordinatorConn.getDB("config")
                           .getCollection("transaction_coordinators")
                           .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
        assert.neq(null, coordDoc);
        assert.sameMembers(coordDoc.participants, expectedParticipantList);
    };

    const checkDecisionIs = function(coordinatorConn, lsid, txnNumber, expectedDecision) {
        let coordDoc = coordinatorConn.getDB("config")
                           .getCollection("transaction_coordinators")
                           .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
        assert.neq(null, coordDoc);
        assert.eq(expectedDecision, coordDoc.decision.decision);
        if (expectedDecision === "commit") {
            assert.neq(null, coordDoc.decision.commitTimestamp);
        } else {
            assert.eq(null, coordDoc.decision.commitTimestamp);
        }
    };

    const checkDocumentDeleted = function(coordinatorConn, lsid, txnNumber) {
        let coordDoc = coordinatorConn.getDB("config")
                           .getCollection("transaction_coordinators")
                           .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
        return null === coordDoc;
    };

    const runCommitThroughMongosInParallelShellExpectSuccess = function() {
        const runCommitExpectSuccessCode = "assert.commandWorked(db.adminCommand({" +
            "commitTransaction: 1," + "lsid: " + tojson(lsid) + "," + "txnNumber: NumberLong(" +
            txnNumber + ")," + "stmtId: NumberInt(0)," + "autocommit: false," + "}));";
        return startParallelShell(runCommitExpectSuccessCode, st.s.port);
    };

    const runCommitThroughMongosInParallelShellExpectAbort = function() {
        const runCommitExpectSuccessCode = "assert.commandFailedWithCode(db.adminCommand({" +
            "commitTransaction: 1," + "lsid: " + tojson(lsid) + "," + "txnNumber: NumberLong(" +
            txnNumber + ")," + "stmtId: NumberInt(0)," + "autocommit: false," + "})," +
            "ErrorCodes.NoSuchTransaction);";
        return startParallelShell(runCommitExpectSuccessCode, st.s.port);
    };

    const startSimulatingNetworkFailures = function(connArray) {
        connArray.forEach(function(conn) {
            assert.commandWorked(conn.adminCommand({
                configureFailPoint: "failCommand",
                mode: {times: 10},
                data: {
                    errorCode: ErrorCodes.NotMaster,
                    failCommands:
                        ["prepareTransaction", "abortTransaction", "commitTransaction"]
                }
            }));
            assert.commandWorked(conn.adminCommand({
                configureFailPoint:
                    "participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic",
                mode: {times: 5}
            }));
            assert.commandWorked(conn.adminCommand({
                configureFailPoint: "participantReturnNetworkErrorForAbortAfterExecutingAbortLogic",
                mode: {times: 5}
            }));
            assert.commandWorked(conn.adminCommand({
                configureFailPoint:
                    "participantReturnNetworkErrorForCommitAfterExecutingCommitLogic",
                mode: {times: 5}
            }));
        });
    };

    const stopSimulatingNetworkFailures = function(connArray) {
        connArray.forEach(function(conn) {
            assert.commandWorked(conn.adminCommand({
                configureFailPoint: "failCommand",
                mode: "off",
            }));
            assert.commandWorked(conn.adminCommand({
                configureFailPoint:
                    "participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic",
                mode: "off"
            }));
            assert.commandWorked(conn.adminCommand({
                configureFailPoint: "participantReturnNetworkErrorForAbortAfterExecutingAbortLogic",
                mode: "off"
            }));
            assert.commandWorked(conn.adminCommand({
                configureFailPoint:
                    "participantReturnNetworkErrorForCommitAfterExecutingCommitLogic",
                mode: "off"
            }));
        });
    };

    const setUp = function() {
        // Create a sharded collection with a chunk on each shard:
        // shard0: [-inf, 0)
        // shard1: [0, 10)
        // shard2: [10, +inf)
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
        assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: coordinator.shardName}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

        // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
        // from the shards starting, aborting, and restarting the transaction due to needing to
        // refresh after the transaction has started.
        assert.commandWorked(coordinator.adminCommand({_flushRoutingTableCacheUpdates: ns}));
        assert.commandWorked(participant1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
        assert.commandWorked(participant2.adminCommand({_flushRoutingTableCacheUpdates: ns}));

        // Start a new transaction by inserting a document onto each shard.
        assert.commandWorked(st.s.getDB(dbName).runCommand({
            insert: collName,
            documents: [{_id: -5}, {_id: 5}, {_id: 15}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }));
    };

    const testCommitProtocol = function(shouldCommit, simulateNetworkFailures) {
        jsTest.log("Testing two-phase " + (shouldCommit ? "commit" : "abort") +
                   " protocol with simulateNetworkFailures: " + simulateNetworkFailures);

        txnNumber++;
        setUp();

        if (!shouldCommit) {
            // Manually abort the transaction on one of the participants, so that the participant
            // fails to prepare.
            assert.commandWorked(participant2.adminCommand({
                abortTransaction: 1,
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(0),
                autocommit: false,
            }));
        }

        if (simulateNetworkFailures) {
            startSimulatingNetworkFailures([participant1, participant2]);
        }

        // Turn on failpoints so that the coordinator hangs after each write it does, so that the
        // test can check that the write happened correctly.
        assert.commandWorked(coordinator.adminCommand({
            configureFailPoint: "hangBeforeWaitingForParticipantListWriteConcern",
            mode: "alwaysOn",
        }));
        assert.commandWorked(coordinator.adminCommand({
            configureFailPoint: "hangBeforeWaitingForDecisionWriteConcern",
            mode: "alwaysOn",
        }));

        // Run commitTransaction through a parallel shell.
        let awaitResult;
        if (shouldCommit) {
            awaitResult = runCommitThroughMongosInParallelShellExpectSuccess();
        } else {
            awaitResult = runCommitThroughMongosInParallelShellExpectAbort();
        }

        // Check that the coordinator wrote the participant list.
        waitForFailpoint("Hit hangBeforeWaitingForParticipantListWriteConcern failpoint",
                         txnNumber);
        checkParticipantListMatches(coordinator, lsid, txnNumber, expectedParticipantList);
        assert.commandWorked(coordinator.adminCommand({
            configureFailPoint: "hangBeforeWaitingForParticipantListWriteConcern",
            mode: "off",
        }));

        // Check that the coordinator wrote the decision.
        waitForFailpoint("Hit hangBeforeWaitingForDecisionWriteConcern failpoint", txnNumber);
        checkParticipantListMatches(coordinator, lsid, txnNumber, expectedParticipantList);
        checkDecisionIs(coordinator, lsid, txnNumber, (shouldCommit ? "commit" : "abort"));
        assert.commandWorked(coordinator.adminCommand({
            configureFailPoint: "hangBeforeWaitingForDecisionWriteConcern",
            mode: "off",
        }));

        // Check that the coordinator deleted its persisted state.
        awaitResult();
        assert.soon(function() {
            return checkDocumentDeleted(coordinator, lsid, txnNumber);
        });

        if (simulateNetworkFailures) {
            stopSimulatingNetworkFailures([participant1, participant2]);
        }

        // Check that the transaction committed or aborted as expected.
        if (!shouldCommit) {
            jsTest.log("Verify that the transaction was aborted on all shards.");
            assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());
        } else {
            jsTest.log("Verify that the transaction was committed on all shards.");
            // Use assert.soon(), because although coordinateCommitTransaction currently blocks
            // until the commit process is fully complete, it will eventually be changed to only
            // block until the decision is *written*, at which point the test can pass the
            // operationTime returned by coordinateCommitTransaction as 'afterClusterTime' in the
            // read to ensure the read sees the transaction's writes (TODO SERVER-37165).
            assert.soon(function() {
                return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
            });
        }

        st.s.getDB(dbName).getCollection(collName).drop();
    };

    testCommitProtocol(false /* test abort */, false /* no network failures */);
    testCommitProtocol(true /* test commit */, false /* no network failures */);
    testCommitProtocol(false /* test abort */, true /* with network failures */);
    testCommitProtocol(true /* test commit */, true /* with network failures */);

    st.stop();

})();
