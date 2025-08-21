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

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    getCoordinatorFailpoints,
    waitForFailpoint,
    flushRoutersAndRefreshShardMetadata,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkDecisionIs,
    runCommitThroughMongosInParallelThread,
} from "jstests/sharding/libs/txn_two_phase_commit_util.js";
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const hangBeforeDeletingCoordinatorDocFpName = "hangBeforeDeletingCoordinatorDoc";

// Lower the transaction timeout for participants, since this test exercises the case where the
// coordinator fails over before writing the participant list and then checks that the
// transaction is aborted on all participants, and the participants will only abort on reaching
// the transaction timeout.
TestData.transactionLifetimeLimitSeconds = 30;

let lsid = {id: UUID()};
let txnNumber = 0;

const runTest = function (sameNodeStepsUpAfterFailover) {
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
        other: {mongosOptions: {verbose: 3}},
    });

    let coordinatorReplSetTest = st.rs0;

    let participant0 = st.shard0;
    let participant1 = st.shard1;
    let participant2 = st.shard2;

    const setUp = function () {
        // Create a sharded collection with a chunk on each shard:
        // shard0: [-inf, 0)
        // shard1: [0, 10)
        // shard2: [10, +inf)
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: participant0.shardName}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
        assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
        assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

        flushRoutersAndRefreshShardMetadata(st, {ns});

        // Start a new transaction by inserting a document onto each shard.
        assert.commandWorked(
            st.s.getDB(dbName).runCommand({
                insert: collName,
                documents: [{_id: -5}, {_id: 5}, {_id: 15}],
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(0),
                startTransaction: true,
                autocommit: false,
            }),
        );
    };

    const cleanUp = function () {
        st.s.getDB(dbName).getCollection(collName).drop();
        clearRawMongoProgramOutput();
    };

    const testCommitProtocol = function (makeAParticipantAbort, failpointData, expectAbortResponse) {
        jsTest.log(
            "Testing commit protocol with sameNodeStepsUpAfterFailover: " +
                sameNodeStepsUpAfterFailover +
                ", makeAParticipantAbort: " +
                makeAParticipantAbort +
                ", expectAbortResponse: " +
                expectAbortResponse +
                ", and failpointData: " +
                tojson(failpointData),
        );

        txnNumber++;
        setUp();

        coordinatorReplSetTest.awaitNodesAgreeOnPrimary();
        let coordPrimary = coordinatorReplSetTest.getPrimary();

        if (makeAParticipantAbort) {
            // Manually abort the transaction on one of the participants, so that the
            // participant fails to prepare.
            assert.commandWorked(
                participant2.adminCommand({
                    abortTransaction: 1,
                    lsid: lsid,
                    txnNumber: NumberLong(txnNumber),
                    stmtId: NumberInt(0),
                    autocommit: false,
                }),
            );
        }

        assert.commandWorked(
            coordPrimary.adminCommand({
                configureFailPoint: failpointData.failpoint,
                mode: "alwaysOn",
                data: failpointData.data ? failpointData.data : {},
            }),
        );

        // Run commitTransaction through a thread.
        let commitThread;
        if (expectAbortResponse) {
            commitThread = runCommitThroughMongosInParallelThread(
                lsid,
                txnNumber,
                st.s.host,
                ErrorCodes.NoSuchTransaction,
            );
        } else {
            commitThread = runCommitThroughMongosInParallelThread(lsid, txnNumber, st.s.host);
        }
        commitThread.start();

        waitForFailpoint("Hit " + failpointData.failpoint + " failpoint", failpointData.numTimesShouldBeHit);

        // Induce the coordinator primary to step down.
        assert.commandWorked(coordPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(coordPrimary.adminCommand({replSetFreeze: 0}));
        assert.commandWorked(coordPrimary.adminCommand({replSetStepUp: 1}));

        // If we're hanging after the decision is written and before it is removed, then we can read
        // the decision.
        const shouldCommit = !expectAbortResponse;
        let commitTimestamp;
        const canReadDecision =
            (failpointData.data || {}).twoPhaseCommitStage === "decision" ||
            failpointData.failpoint === hangBeforeDeletingCoordinatorDocFpName;
        let hangBeforeDeletingCoordinatorDocFp;
        if (canReadDecision && shouldCommit) {
            commitTimestamp = checkDecisionIs(coordPrimary, lsid, txnNumber, "commit");
        } else if (shouldCommit) {
            // We're hanging before the decision is written, so we need to read the decision later
            // but before it's deleted.
            hangBeforeDeletingCoordinatorDocFp = configureFailPoint(
                coordPrimary,
                hangBeforeDeletingCoordinatorDocFpName,
                {},
                "alwaysOn",
            );
        }

        assert.commandWorked(
            coordPrimary.adminCommand({
                configureFailPoint: failpointData.failpoint,
                mode: "off",
            }),
        );

        if (!canReadDecision && shouldCommit) {
            // Wait to delete the coordinator doc, then read the commitTimestamp.
            hangBeforeDeletingCoordinatorDocFp.wait();
            commitTimestamp = checkDecisionIs(coordPrimary, lsid, txnNumber, "commit");
            hangBeforeDeletingCoordinatorDocFp.off();
        }

        // The router should retry commitTransaction against the new primary.
        commitThread.join();

        // Check that the transaction committed or aborted as expected.
        if (expectAbortResponse) {
            jsTest.log("Verify that the transaction was aborted on all shards.");
            assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());
        } else {
            jsTest.log("Verify that the transaction was committed on all shards.");
            const res = assert.commandWorked(
                st.s.getDB(dbName).runCommand({
                    find: collName,
                    readConcern: {level: "majority", afterClusterTime: commitTimestamp},
                    maxTimeMS: 10000,
                }),
            );
            assert.eq(3, res.cursor.firstBatch.length);
        }

        cleanUp();
    };

    const testCommitProtocolWithRetry = function (makeAParticipantAbort, failpointData, expectAbortResponse) {
        const maxIterations = 5;
        var numIterations = 0;

        while (numIterations < maxIterations) {
            try {
                testCommitProtocol(makeAParticipantAbort, failpointData, expectAbortResponse);
                break;
            } catch (err) {
                if (numIterations == maxIterations - 1 || !TxnUtil.isTransientTransactionError(err)) {
                    throw err;
                }

                cleanUp();
                numIterations += 1;
            }

            jsTest.log("Received an error with label TransientTransactionError. Retry: " + numIterations);
        }
    };

    //
    // Run through all the failpoints when one participant responds to prepare with vote abort.
    //

    failpointDataArr.forEach(function (failpointData) {
        testCommitProtocolWithRetry(
            true /* make a participant abort */,
            failpointData,
            true /* expect abort decision */,
        );
    });

    //
    // Run through all the failpoints when all participants respond to prepare with vote commit.
    //

    failpointDataArr.forEach(function (failpointData) {
        // Note: If the coordinator fails over before making the participant list durable,
        // the transaction will abort even if all participants could have committed. This is
        // a property of the coordinator only, and would be true even if a participant's
        // in-progress transaction could survive failover.
        let expectAbort =
            failpointData.failpoint == "hangBeforeWritingParticipantList" ||
            (failpointData.failpoint == "hangWhileTargetingLocalHost" &&
                failpointData.data.twoPhaseCommitStage == "prepare");
        testCommitProtocolWithRetry(false /* make a participant abort */, failpointData, expectAbort);
    });
    st.stop();
};

const failpointDataArr = getCoordinatorFailpoints();

runTest(true /* same node always steps up after stepping down */);
runTest(false /* different node always steps up after stepping down */);
