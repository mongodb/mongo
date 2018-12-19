/**
 * Exercises transaction coordinator failover by inducing stepdowns at specific points and ensuring
 * the transaction completes and the router is able to learn the decision by retrying
 * coordinateCommitTransaction.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
    'use strict';

    load('jstests/sharding/libs/sharded_transactions_helpers.js');

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    // Lower the transaction timeout for participants, since this test exercises the case where the
    // coordinator fails over before writing the participant list and then checks that the
    // transaction is aborted on all participants, and the participants will only abort on reaching
    // the transaction timeout.
    TestData.transactionLifetimeLimitSeconds = 15;

    let failpointCounter = 0;

    const runTest = function(sameNodeStepsUpAfterFailover) {

        jsTest.log("Testing all scenarios with sameNodeStepsUpAfterFailover: " +
                   sameNodeStepsUpAfterFailover);

        let stepDownSecs;  // The amount of time the node has to wait before becoming primary again.
        let numCoordinatorNodes;
        if (sameNodeStepsUpAfterFailover) {
            numCoordinatorNodes = 1;
            stepDownSecs = 1;
        } else {
            numCoordinatorNodes = 3;
            stepDownSecs = 3;
        }

        let st = new ShardingTest({
            shards: 3,                    // number of *regular shards*
            config: numCoordinatorNodes,  // number of replica set *nodes* in *config shard*
            causallyConsistent: true,
            other: {
                mongosOptions: {
                    // This failpoint is needed because it is not yet possible to step down a node
                    // with a prepared transaction.
                    setParameter:
                        {"failpoint.sendCoordinateCommitToConfigServer": "{'mode': 'alwaysOn'}"},
                    verbose: 3
                },
                configOptions: {
                    // This failpoint is needed because of the other failpoint: the config server
                    // will not have a local participant, so coordinateCommitTransaction cannot fall
                    // back to recovering the decision from the local participant.
                    setParameter: {"failpoint.doNotForgetCoordinator": "{'mode': 'alwaysOn'}"},
                }
            }
        });

        let coordinatorReplSetTest = st.configRS;
        let participant0 = st.shard0;
        let participant1 = st.shard1;
        let participant2 = st.shard2;

        let expectedParticipantList =
            [participant0.shardName, participant1.shardName, participant2.shardName];

        let lsid = {id: UUID()};
        let txnNumber = 0;

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

        const setUp = function() {
            // Create a sharded collection with a chunk on each shard:
            // shard0: [-inf, 0)
            // shard1: [0, 10)
            // shard2: [10, +inf)
            assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
            assert.commandWorked(
                st.s.adminCommand({movePrimary: dbName, to: participant0.shardName}));
            assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
            assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
            assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
            assert.commandWorked(
                st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
            assert.commandWorked(
                st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

            // These forced refreshes are not strictly necessary; they just prevent extra TXN log
            // lines from the shards starting, aborting, and restarting the transaction due to
            // needing to refresh after the transaction has started.
            assert.commandWorked(participant0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
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

        const testCommitProtocol = function(makeAParticipantAbort, failpoint, expectAbortResponse) {
            jsTest.log("Testing commit protocol with makeAParticipantAbort: " +
                       makeAParticipantAbort + ", failpoint: " + failpoint +
                       ", and expectAbortResponse: " + expectAbortResponse);

            txnNumber++;
            setUp();

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

            coordinatorReplSetTest.awaitNodesAgreeOnPrimary();
            let coordPrimary = coordinatorReplSetTest.getPrimary();

            assert.commandWorked(coordPrimary.adminCommand({
                configureFailPoint: failpoint,
                mode: "alwaysOn",
            }));

            // Run commitTransaction through a parallel shell.
            let awaitResult;
            if (expectAbortResponse) {
                awaitResult = runCommitThroughMongosInParallelShellExpectAbort();
            } else {
                awaitResult = runCommitThroughMongosInParallelShellExpectSuccess();
            }

            // Wait for the desired failpoint to be hit.
            waitForFailpoint("Hit " + failpoint + " failpoint", failpointCounter);

            // Induce the coordinator primary to step down.
            const stepDownResult = assert.throws(function() {
                coordPrimary.adminCommand({replSetStepDown: stepDownSecs, force: true});
            });
            assert(isNetworkError(stepDownResult),
                   'Expected exception from stepping down coordinator primary ' +
                       coordPrimary.host + ': ' + tojson(stepDownResult));
            assert.commandWorked(coordPrimary.adminCommand({
                configureFailPoint: failpoint,
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
                // block until the decision is *written*, at which point the test can pass the
                // operationTime returned by coordinateCommitTransaction as 'afterClusterTime' in
                // the read to ensure the read sees the transaction's writes (TODO SERVER-37165).
                assert.soon(function() {
                    return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
                });
            }

            st.s.getDB(dbName).getCollection(collName).drop();
        };

        //
        // Run through all the failpoints when one participant responds to prepare with vote abort.
        //

        ++failpointCounter;

        testCommitProtocol(true /* make a participant abort */,
                           "hangBeforeWritingParticipantList",
                           true /* expect abort decision */);
        testCommitProtocol(true /* make a participant abort */,
                           "hangBeforeWritingDecision",
                           true /* expect abort decision */);
        testCommitProtocol(true /* make a participant abort */,
                           "hangBeforeDeletingCoordinatorDoc",
                           true /* expect abort decision */);

        //
        // Run through all the failpoints when all participants respond to prepare with vote commit.
        //

        ++failpointCounter;

        // Note: If the coordinator fails over before making the participant list durable, the
        // transaction will abort even if all participants could have committed. Further note that
        // this is a property of the coordinator only - in general, the coordinator is co-located
        // with a participant and in 4.2, participants abort if they fail over before prepare. This
        // is really testing that even if the participant's unprepared transaction was able to
        // survive failover at some future time (for example, in the near future for read-only
        // transactions, or in the far future if we add support for multi-master), then the
        // transaction would nevertheless abort due to the design of the coordinator.
        testCommitProtocol(false /* all participants can commit */,
                           "hangBeforeWritingParticipantList",
                           true /* expect abort decision */);

        testCommitProtocol(false /* all participants can commit */,
                           "hangBeforeWritingDecision",
                           false /* expect commit decision */);
        testCommitProtocol(
            false /* all participants can commit */, "hangBeforeDeletingCoordinatorDoc", false
            /* expect commit decision */);

        st.stop();
    };

    // Same node *always* steps back up after stepping down.
    runTest(true);

    // Same or different node can step back up after stepping down (but most likely a different node
    // will).
    runTest(false);

})();
