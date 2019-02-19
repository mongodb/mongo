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

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    // Lower the transaction timeout for participants, since this test exercises the case where the
    // coordinator fails over before writing the participant list and then checks that the
    // transaction is aborted on all participants, and the participants will only abort on reaching
    // the transaction timeout.
    TestData.transactionLifetimeLimitSeconds = 15;

    let lsid = {id: UUID()};
    let txnNumber = 0;

    const runTest = function(sameNodeStepsUpAfterFailover, overrideCoordinatorToBeConfigServer) {
        let stepDownSecs;  // The amount of time the node has to wait before becoming primary again.
        let numCoordinatorNodes;
        if (sameNodeStepsUpAfterFailover) {
            numCoordinatorNodes = 1;
            stepDownSecs = 1;
        } else {
            numCoordinatorNodes = 3;
            stepDownSecs = 3;
        }

        let st, coordinatorReplSetTest;
        if (overrideCoordinatorToBeConfigServer) {
            st = new ShardingTest({
                shards: 3,                    // number of *regular shards*
                config: numCoordinatorNodes,  // number of replica set *nodes* in *config shard*
                causallyConsistent: true,
                other: {
                    mongosOptions: {
                        // This failpoint is needed because it is not yet possible to step down a
                        // node with a prepared transaction.
                        setParameter: {
                            "failpoint.sendCoordinateCommitToConfigServer": "{'mode': 'alwaysOn'}"
                        },
                        verbose: 3
                    },
                    configOptions: {
                        // This failpoint is needed because of the other failpoint: the config
                        // server will not have a local participant, so coordinateCommitTransaction
                        // cannot fall back to recovering the decision from the local participant.
                        setParameter: {"failpoint.doNotForgetCoordinator": "{'mode': 'alwaysOn'}"},
                    }
                }
            });

            coordinatorReplSetTest = st.configRS;
        } else {
            st = new ShardingTest({
                shards: 3,
                rs0: {nodes: numCoordinatorNodes},
                causallyConsistent: true,
                other: {mongosOptions: {verbose: 3}}
            });

            coordinatorReplSetTest = st.rs0;
        }

        let participant0 = st.shard0;
        let participant1 = st.shard1;
        let participant2 = st.shard2;

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

        const testCommitProtocol = function(
            makeAParticipantAbort, failpointData, expectAbortResponse) {
            jsTest.log("Testing commit protocol with sameNodeStepsUpAfterFailover: " +
                       sameNodeStepsUpAfterFailover + ", overrideCoordinatorToBeConfigServer: " +
                       overrideCoordinatorToBeConfigServer + ", makeAParticipantAbort: " +
                       makeAParticipantAbort + ", expectAbortResponse: " + expectAbortResponse +
                       ", and failpointData: " + tojson(failpointData));

            txnNumber++;
            setUp();

            coordinatorReplSetTest.awaitNodesAgreeOnPrimary();
            let coordPrimary = coordinatorReplSetTest.getPrimary();

            if (makeAParticipantAbort) {
                // In order to test coordinator failover for a coordinator colocated with a
                // participant, the participant colocated with the coordinator must fail to prepare,
                // because prepare does not yet support failover.
                let nodeToAbort = overrideCoordinatorToBeConfigServer ? participant2 : coordPrimary;

                // Manually abort the transaction on one of the participants, so that the
                // participant fails to prepare.
                assert.commandWorked(nodeToAbort.adminCommand({
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

            // TODO(SERVER-39754): Rewrite this entire test as a unit-test instead
            var numTimesShouldBeHit = failpointData.numTimesShouldBeHit;
            if (!overrideCoordinatorToBeConfigServer &&  // Coordinator is co-located with a
                                                         // participant
                (failpointData.failpoint == "hangWhileTargetingRemoteHost" &&
                 !failpointData.skip) &&  // We are testing the prepare phase
                makeAParticipantAbort) {  // The local participant will vote abort
                // Wait for those two to be scheduled as well
                numTimesShouldBeHit += 2;
            }
            waitForFailpoint("Hit " + failpointData.failpoint + " failpoint", numTimesShouldBeHit);

            // Induce the coordinator primary to step down.
            assert.commandWorked(
                coordPrimary.adminCommand({replSetStepDown: stepDownSecs, force: true}));
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
                // block until the decision is *written*, at which point the test can pass the
                // operationTime returned by coordinateCommitTransaction as 'afterClusterTime' in
                // the read to ensure the read sees the transaction's writes (TODO SERVER-37165).
                assert.soon(function() {
                    return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
                });
            }

            st.s.getDB(dbName).getCollection(collName).drop();
            clearRawMongoProgramOutput();
        };

        //
        // Run through all the failpoints when one participant responds to prepare with vote abort.
        //

        failpointDataArr.forEach(function(failpointData) {
            if (overrideCoordinatorToBeConfigServer &&
                failpointData.failpoint == "hangWhileTargetingLocalHost") {
                // If the coordinator is overridden to be the config server, it will never target
                // itself, so don't test the target local path.
                return;
            }
            testCommitProtocol(true /* make a participant abort */,
                               failpointData,
                               true /* expect abort decision */);
        });

        //
        // Run through all the failpoints when all participants respond to prepare with vote commit.
        //

        // We only test two-phase commit (as opposed to two-phase abort) if the coordinator is
        // overridden to be the config server, because prepare does not yet support failover.
        if (overrideCoordinatorToBeConfigServer) {
            failpointDataArr.forEach(function(failpointData) {
                if (failpointData.failpoint == "hangWhileTargetingLocalHost") {
                    // If the coordinator is overridden to be the config server, it will never
                    // target itself, so don't test the target local path.
                    return;
                }
                // Note: If the coordinator fails over before making the participant list durable,
                // the transaction will abort even if all participants could have committed. This is
                // a property of the coordinator only, and would be true even if a participant's
                // in-progress transaction could survive failover.
                let expectAbort =
                    (failpointData.failpoint == "hangBeforeWritingParticipantList") || false;
                testCommitProtocol(
                    false /* make a participant abort */, failpointData, expectAbort);
            });
        }
        st.stop();
    };

    const failpointDataArr = getCoordinatorFailpoints();

    //
    // Coordinator is co-located with a participant
    //

    runTest(true /* same node always steps up after stepping down */, false);
    runTest(false /* same node always steps up after stepping down */, false);

    //
    // Override coordinator to be config server
    //

    // If the coordinator is overridden to be the config server, it will send a remote request
    // rather than local request, so there is one additional remote request.
    failpointDataArr.forEach(function(failpointData) {
        if (failpointData.failpoint == "hangWhileTargetingRemoteHost") {
            failpointData.numTimesShouldBeHit++;
            if (failpointData.skip) {
                failpointData.skip++;
            }
        }
    });

    runTest(true /* same node always steps up after stepping down */, true);
    runTest(false /* same node always steps up after stepping down */, true);
})();
