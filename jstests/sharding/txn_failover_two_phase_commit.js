/**
 * Exercises transaction coordinator failover by inducing stepdowns at specific points and ensuring
 * the transaction completes and the router is able to learn the decision by retrying
 * coordinateCommitTransaction.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction]
 */

(function() {
    'use strict';

    load("jstests/libs/check_log.js");

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    let st = new ShardingTest({
        shards: 3,
        causallyConsistent: true,
        other: {
            mongosOptions: {
                // This failpoint is needed because it is not yet possible to step down a node with
                // a prepared transaction.
                setParameter:
                    {"failpoint.sendCoordinateCommitToConfigServer": "{'mode': 'alwaysOn'}"},
                verbose: 3
            },
            configOptions:
                // This failpoint is needed because of the other failpoint: the config server will
                // not have a local participant, so coordinateCommitTransaction cannot fall back to
                // recovering the decision from the local participant.
                {setParameter: {"failpoint.doNotForgetCoordinator": "{'mode': 'alwaysOn'}"}}
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
        assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: participant0.shardName}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

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

    const testCommitProtocol = function(shouldCommit, failpoint) {
        jsTest.log("Testing two-phase " + (shouldCommit ? "commit" : "abort") +
                   " protocol with failover at failpoint: " + failpoint);

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

        coordinatorReplSetTest.awaitNodesAgreeOnPrimary();
        let coordPrimary = coordinatorReplSetTest.getPrimary();

        assert.commandWorked(coordPrimary.adminCommand({
            configureFailPoint: failpoint,
            mode: "alwaysOn",
        }));

        // Run commitTransaction through a parallel shell.
        let awaitResult;
        if (shouldCommit) {
            awaitResult = runCommitThroughMongosInParallelShellExpectSuccess();
        } else {
            awaitResult = runCommitThroughMongosInParallelShellExpectAbort();
        }

        // Wait for the desired failpoint to be hit.
        checkLog.contains(coordPrimary, "Hit " + failpoint + " failpoint");

        // Induce the coordinator primary to step down.
        const stepDownResult = assert.throws(function() {
            // The amount of time the node has to wait before becoming primary again.
            const stepDownSecs = 1;
            coordPrimary.adminCommand({replSetStepDown: stepDownSecs, force: true});
        });
        assert(isNetworkError(stepDownResult),
               'Expected exception from stepping down coordinator primary ' + coordPrimary.host +
                   ': ' + tojson(stepDownResult));
        assert.commandWorked(coordPrimary.adminCommand({
            configureFailPoint: failpoint,
            mode: "off",
        }));

        // The router should retry commitTransaction against the new primary.
        awaitResult();

        // Clear RAMLog on this node so that we can use checkLog again. Otherwise, the next
        // checkLog against this node will see the log line from the previous test case and not
        // wait for a new log line.
        coordinatorReplSetTest.restart(coordPrimary);

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

    testCommitProtocol(false /* test abort */, "hangBeforeWritingDecision");
    testCommitProtocol(false /* test abort */, "hangBeforeDeletingCoordinatorDoc");

    testCommitProtocol(true /* test commit */, "hangBeforeWritingDecision");
    testCommitProtocol(true /* test commit */, "hangBeforeDeletingCoordinatorDoc");

    st.stop();

})();
