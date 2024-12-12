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

import {
    getCoordinatorFailpoints,
    waitForFailpoint,
    flushRoutersAndRefreshShardMetadata,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    runCommitThroughMongosInParallelThread
} from 'jstests/sharding/libs/txn_two_phase_commit_util.js';
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

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
    let coordinatorReplSetConfig;

    if (sameNodeStepsUpAfterFailover) {
        coordinatorReplSetConfig = [{}];
    } else {
        // We are making one of the secondaries non-electable to ensure
        // that elections always result in a winner (see SERVER-42234)
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

    const setUp = function() {
        // Create a sharded collection with a chunk on each shard:
        // shard0: [-inf, 0)
        // shard1: [0, 10)
        // shard2: [10, +inf)
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: participant0.shardName}));
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
            mode: "alwaysOn",
            data: failpointData.data ? failpointData.data : {},
        }));

        // Run commitTransaction through a thread.
        let commitThread;
        if (expectAbortResponse) {
            commitThread = runCommitThroughMongosInParallelThread(
                lsid, txnNumber, st.s.host, ErrorCodes.NoSuchTransaction);
        } else {
            commitThread = runCommitThroughMongosInParallelThread(lsid, txnNumber, st.s.host);
        }
        commitThread.start();

        waitForFailpoint("Hit " + failpointData.failpoint + " failpoint",
                         failpointData.numTimesShouldBeHit);

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
        commitThread.join();

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
                    !TxnUtil.isTransientTransactionError(err)) {
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
            (failpointData.failpoint == "hangWhileTargetingLocalHost" &&
             (failpointData.data.twoPhaseCommitStage == "prepare")) ||
            false;
        testCommitProtocolWithRetry(
            false /* make a participant abort */, failpointData, expectAbort);
    });
    st.stop();
};

const failpointDataArr = getCoordinatorFailpoints();

runTest(true /* same node always steps up after stepping down */, false);
runTest(false /* same node always steps up after stepping down */, false);
