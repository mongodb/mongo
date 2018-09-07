/**
 * The equivalent of jstests/core/txns/statement_ids_accepted.js, but for transactions commands that
 * are only legal in sharded clusters (e.g., the two phase commit commands).
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "foo";

    const txnNumber = 0;
    const lsid = {id: UUID()};

    const checkCoordinatorCommandsAccepted = function(conn) {
        const testDB = conn.getDB(dbName);

        // Set up the in-memory coordinator state on the node by simulating a transaction statement
        // with "coordinator: true" against the node.
        assert.commandWorked(testDB.runCommand({
            find: collName,
            readConcern: {level: "snapshot"},
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
            coordinator: true
        }));

        assert.commandWorked(conn.adminCommand({
            coordinateCommitTransaction: 1,
            participants: [{shardId: "voteCommitDummy"}, {shardId: "voteAbortDummy"}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false
        }));

        assert.commandWorked(conn.adminCommand({
            voteCommitTransaction: 1,
            shardId: "voteCommitDummy",
            prepareTimestamp: Timestamp(0, 0),
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false
        }));

        assert.commandWorked(conn.adminCommand({
            voteAbortTransaction: 1,
            shardId: "voteAbortDummy",
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false
        }));

        // Manually abort the transaction on the coordinator. This is required to release the
        // transaction's locks before the post-test validate hook runs.
        assert.commandWorked(conn.adminCommand({
            abortTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        }));
    };

    const checkCoordinatorCommandsRejected = function(conn, expectedErrorCode) {
        assert.commandFailedWithCode(conn.adminCommand({
            coordinateCommitTransaction: 1,
            participants: [{shardId: "voteCommitDummy"}, {shardId: "voteAbortDummy"}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false
        }),
                                     expectedErrorCode);

        assert.commandFailedWithCode(conn.adminCommand({
            voteCommitTransaction: 1,
            shardId: "voteCommitDummy",
            prepareTimestamp: Timestamp(0, 0),
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false
        }),
                                     expectedErrorCode);

        assert.commandFailedWithCode(conn.adminCommand({
            voteAbortTransaction: 1,
            shardId: "voteAbortDummy",
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false
        }),
                                     expectedErrorCode);
    };

    const checkCoordinatorCommandsAgainstNonAdminDbRejected = function(conn) {
        const testDB = conn.getDB(dbName);

        assert.commandFailedWithCode(testDB.runCommand({
            coordinateCommitTransaction: 1,
            participants: [{shardId: "voteCommitDummy"}, {shardId: "voteAbortDummy"}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false
        }),
                                     ErrorCodes.Unauthorized);

        assert.commandFailedWithCode(testDB.runCommand({
            voteCommitTransaction: 1,
            shardId: "voteCommitDummy",
            prepareTimestamp: Timestamp(0, 0),
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false
        }),
                                     ErrorCodes.Unauthorized);

        assert.commandFailedWithCode(testDB.runCommand({
            voteAbortTransaction: 1,
            shardId: "voteAbortDummy",
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false
        }),
                                     ErrorCodes.Unauthorized);
    };

    const st = new ShardingTest({shards: 1});

    jsTest.log("Verify that config servers and shard servers accept coordinator commands");
    checkCoordinatorCommandsAccepted(st.rs0.getPrimary());
    checkCoordinatorCommandsAccepted(st.configRS.getPrimary());

    jsTest.log("Verify that coordinator commands are only accepted against the admin database");
    checkCoordinatorCommandsAgainstNonAdminDbRejected(st.rs0.getPrimary());
    checkCoordinatorCommandsAgainstNonAdminDbRejected(st.configRS.getPrimary());

    st.stop();

    jsTest.log(
        "Verify that a shard server that has not yet been added to a cluster does not accept coordinator commands");
    const shardsvrReplSet = new ReplSetTest({nodes: 1, nodeOptions: {shardsvr: ""}});
    shardsvrReplSet.startSet();
    shardsvrReplSet.initiate();
    checkCoordinatorCommandsRejected(shardsvrReplSet.getPrimary(),
                                     ErrorCodes.ShardingStateNotInitialized);
    shardsvrReplSet.stopSet();

    jsTest.log(
        "Verify that a non-config server, non-shard server does not accept coordinator commands");
    const standaloneReplSet = new ReplSetTest({nodes: 1});
    standaloneReplSet.startSet();
    standaloneReplSet.initiate();
    checkCoordinatorCommandsRejected(standaloneReplSet.getPrimary(), ErrorCodes.NoShardingEnabled);
    standaloneReplSet.stopSet();

})();
