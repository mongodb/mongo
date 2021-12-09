/**
 * Exercises transaction coordinator failover by inducing stepdowns at specific points and ensuring
 * the transaction completes and the router is able to learn the decision by retrying
 * coordinateCommitTransaction.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test causes failovers on a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');
load('jstests/libs/parallel_shell_helpers.js');

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Lower the transaction timeout for participants, since this test exercises the case where the
// coordinator fails over before writing the participant list and then checks that the
// transaction is aborted on all participants, and the participants will only abort on reaching
// the transaction timeout.
TestData.transactionLifetimeLimitSeconds = 30;

let lsid = {id: UUID()};
let txnNumber = 0;

const runTest = function(sameNodeStepsUpAfterFailover) {
    let stepDownSecs;  // The amount of time the node has to wait before becoming primary again.
    let coordinatorReplSetConfig;

    if (sameNodeStepsUpAfterFailover) {
        stepDownSecs = 1;
        coordinatorReplSetConfig = [{}];
    } else {
        // We are making one of the secondaries non-electable to ensure
        // that elections always result in a winner (see SERVER-42234)
        stepDownSecs = 3;
        coordinatorReplSetConfig = [{}, {}, {rsConfig: {priority: 0}}];
    }

    let st = new ShardingTest({
        shards: 3,
        rs0: {nodes: coordinatorReplSetConfig},
        causallyConsistent: true,
        other: {mongosOptions: {verbose: 3}}
    });

    let coordinatorReplSetTest = st.rs0;

    let participant0 = st.shard0;
    let participant1 = st.shard1;
    let participant2 = st.shard2;

    const runCommitThroughMongosInParallelShellExpectSuccess = function() {
        return startParallelShell(
            funWithArgs((passed_lsid, passed_txnNumber) => {
                try {
                    assert.commandWorked(db.adminCommand({
                        commitTransaction: 1,
                        lsid: passed_lsid,
                        txnNumber: NumberLong(passed_txnNumber),
                        stmtId: NumberInt(0),
                        autocommit: false,
                    }));
                } catch (err) {
                    if ((err.hasOwnProperty('errorLabels') &&
                         err.errorLabels.includes('TransientTransactionError'))) {
                        quit(err.code);
                    } else {
                        throw err;
                    }
                }
            }, lsid, txnNumber), st.s.port);
    };

    const runCommitThroughMongosInParallelShellExpectAbort = function() {
        const runCommitExpectSuccessCode = "assert.commandFailedWithCode(db.adminCommand({" +
            "commitTransaction: 1," +
            "lsid: " + tojson(lsid) + "," +
            "txnNumber: NumberLong(" + txnNumber + ")," +
            "stmtId: NumberInt(0)," +
            "autocommit: false," +
            "})," +
            "ErrorCodes.NoSuchTransaction);";
        return startParallelShell(runCommitExpectSuccessCode, st.s.port);
    };

    const setUp = function() {
        // Create a sharded collection with a chunk on each shard:
        // shard0: [-inf, 0)
        // shard1: [0, 10)
        // shard2: [10, +inf)
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
        assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: participant0.shardName}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

        flushRoutersAndRefreshShardMetadata(st, {ns});

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

    const cleanUp = function() {
        st.s.getDB(dbName).getCollection(collName).drop();
        clearRawMongoProgramOutput();
    };

    const testCommitProtocol = function(makeAParticipantAbort, failpointData, expectAbortResponse) {
        jsTest.log("Testing commit protocol with sameNodeStepsUpAfterFailover: " +
                   sameNodeStepsUpAfterFailover + ", makeAParticipantAbort: " +
                   makeAParticipantAbort + ", expectAbortResponse: " + expectAbortResponse +
                   ", and failpointData: " + tojson(failpointData));

        txnNumber++;
        setUp();

        coordinatorReplSetTest.awaitNodesAgreeOnPrimary();
        let coordPrimary = coordinatorReplSetTest.getPrimary();

        if (makeAParticipantAbort) {
            // Manually abort the transaction on one of the participants, so that the
            // participant fails to prepare.
            assert.commandWorked(participant2.adminCommand({
                abortTransaction: 1,
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(0),
                autocommit: false,
            }));
        }

        assert.commandWorked(coordPrimary.adminCommand({
            configureFailPoint: failpointData.failpoint,
            mode: {skip: (failpointData.skip ? failpointData.skip : 0)},
        }));

        // Run commitTransaction through a parallel shell.
        let awaitResult;
        if (expectAbortResponse) {
            awaitResult = runCommitThroughMongosInParallelShellExpectAbort();
        } else {
            awaitResult = runCommitThroughMongosInParallelShellExpectSuccess();
        }

        var numTimesShouldBeHit = failpointData.numTimesShouldBeHit;
        if ((failpointData.failpoint == "hangWhileTargetingLocalHost" &&
             !failpointData.skip) &&  // We are testing the prepare phase
            makeAParticipantAbort) {  // A remote participant will vote abort
            // Wait for the abort to the local host to be scheduled as well.
            numTimesShouldBeHit++;
        }

        waitForFailpoint("Hit " + failpointData.failpoint + " failpoint", numTimesShouldBeHit);

        // Induce the coordinator primary to step down.
        assert.commandWorked(
            coordPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(coordPrimary.adminCommand({replSetFreeze: 0}));
        assert.commandWorked(coordPrimary.adminCommand({replSetStepUp: 1}));

        assert.commandWorked(coordPrimary.adminCommand({
            configureFailPoint: failpointData.failpoint,
            mode: "off",
        }));

        // The router should retry commitTransaction against the new primary.
        awaitResult();

        // Check that the transaction committed or aborted as expected.
        if (expectAbortResponse) {
            jsTest.log("Verify that the transaction was aborted on all shards.");
            assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());
        } else {
            jsTest.log("Verify that the transaction was committed on all shards.");
            // Use assert.soon(), because although coordinateCommitTransaction currently blocks
            // until the commit process is fully complete, it will eventually be changed to only
            // block until the decision is *written*, so the documents may not be visible
            // immediately.
            assert.soon(function() {
                return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
            });
        }

        cleanUp();
    };

    const testCommitProtocolWithRetry = function(
        makeAParticipantAbort, failpointData, expectAbortResponse) {
        const maxIterations = 5;
        var numIterations = 0;

        while (numIterations < maxIterations) {
            try {
                testCommitProtocol(makeAParticipantAbort, failpointData, expectAbortResponse);
                break;
            } catch (err) {
                if (numIterations == maxIterations - 1 ||
                    !(err.message.includes("[0] != [251] are not equal"))) {
                    throw err;
                }

                cleanUp();
                numIterations += 1;
            }

            jsTest.log("Received an error with label TransientTransactionError. Retry: " +
                       numIterations);
        }
    };

    //
    // Run through all the failpoints when one participant responds to prepare with vote abort.
    //

    failpointDataArr.forEach(function(failpointData) {
        testCommitProtocolWithRetry(
            true /* make a participant abort */, failpointData, true /* expect abort decision */);
    });

    //
    // Run through all the failpoints when all participants respond to prepare with vote commit.
    //

    failpointDataArr.forEach(function(failpointData) {
        // Note: If the coordinator fails over before making the participant list durable,
        // the transaction will abort even if all participants could have committed. This is
        // a property of the coordinator only, and would be true even if a participant's
        // in-progress transaction could survive failover.
        let expectAbort = (failpointData.failpoint == "hangBeforeWritingParticipantList") ||
            (failpointData.failpoint == "hangWhileTargetingLocalHost" && !failpointData.skip) ||
            false;
        testCommitProtocolWithRetry(
            false /* make a participant abort */, failpointData, expectAbort);
    });
    st.stop();
};

const failpointDataArr = getCoordinatorFailpoints();

runTest(true /* same node always steps up after stepping down */, false);
runTest(false /* same node always steps up after stepping down */, false);
})();
