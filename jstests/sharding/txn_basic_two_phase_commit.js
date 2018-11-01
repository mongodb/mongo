/**
 * Exercises the coordinator commands logic by simulating a basic two phase commit and basic two
 * phase abort.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction]
 */
(function() {
    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    let st = new ShardingTest({shards: 3, causallyConsistent: true});

    let coordinator = st.shard0;
    let participant1 = st.shard1;
    let participant2 = st.shard2;

    let lsid = {id: UUID()};
    let txnNumber = 0;

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

    const testTwoPhaseAbort = function(simulateNetworkFailures) {
        jsTest.log("Testing two-phase abort with simulateNetworkFailures: " +
                   simulateNetworkFailures);

        txnNumber++;
        setUp();

        // Manually abort the transaction on one of the participants, so that the participant fails
        // to prepare.
        assert.commandWorked(participant2.adminCommand({
            abortTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        }));

        if (simulateNetworkFailures) {
            startSimulatingNetworkFailures([participant1, participant2, coordinator]);
        }
        assert.commandFailedWithCode(st.s.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        }),
                                     ErrorCodes.NoSuchTransaction);
        if (simulateNetworkFailures) {
            stopSimulatingNetworkFailures([participant1, participant2, coordinator]);
        }

        // Verify that the transaction was aborted on all shards.
        assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());
        st.s.getDB(dbName).getCollection(collName).drop();
    };

    const testTwoPhaseCommit = function(simulateNetworkFailures) {
        jsTest.log("Testing two-phase commit with simulateNetworkFailures: " +
                   simulateNetworkFailures);

        txnNumber++;
        setUp();

        if (simulateNetworkFailures) {
            startSimulatingNetworkFailures([participant1, participant2, coordinator]);
        }
        assert.commandWorked(st.s.adminCommand({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        }));
        if (simulateNetworkFailures) {
            stopSimulatingNetworkFailures([participant1, participant2, coordinator]);
        }

        // Verify that the transaction was committed on all shards.
        // Use assert.soon(), because although coordinateCommitTransaction currently blocks until
        // the commit process is fully complete, it will eventually be changed to only block until
        // the decision is *written*, at which point the test can pass the operationTime returned by
        // coordinateCommitTransaction as 'afterClusterTime' in the read to ensure the read sees the
        // transaction's writes (TODO SERVER-37165).
        assert.soon(function() {
            return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
        });

        st.s.getDB(dbName).getCollection(collName).drop();
    };

    testTwoPhaseAbort(false);
    testTwoPhaseCommit(false);
    testTwoPhaseAbort(true);
    testTwoPhaseCommit(true);

    st.stop();

})();
