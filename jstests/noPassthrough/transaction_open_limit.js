/**
 * Tests that the server enforces a limit on the number of concurrently open multi-document
 * transactions via the maxConcurrentMultiDocumentTransactions server parameter.
 *
 * @tags: [
 *   uses_transactions,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

function fillToLimit(conn, collName, limit, idPrefix) {
    const sessions = [];
    for (let i = 0; i < limit; i++) {
        const session = conn.startSession();
        session.startTransaction();
        assert.commandWorked(
            session.getDatabase("test")[collName].insert({_id: `${idPrefix}_${i}`}));
        sessions.push(session);
    }
    return sessions;
}

function abortAll(sessions) {
    for (const session of sessions) {
        session.abortTransaction();
        session.endSession();
    }
}

function runReplicaSetTests() {
    const limit = 100;
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const collName = "transaction_open_limit";
    assert.commandWorked(primary.getDB("test").createCollection(collName));
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, maxConcurrentMultiDocumentTransactions: limit}));

    jsTest.log("rejects transactions beyond the limit and allows after abort");
    {
        const sessions = fillToLimit(primary, collName, limit, "basic");

        {
            const session = primary.startSession();
            session.startTransaction();
            const res = session.getDatabase("test")[collName].insert({_id: "over_limit"});
            assert.commandFailedWithCode(res, ErrorCodes.TooManyOpenTransactions);
            session.endSession();
        }

        sessions[0].abortTransaction();
        sessions[0].endSession();
        sessions.shift();

        {
            const session = primary.startSession();
            session.startTransaction();
            assert.commandWorked(
                session.getDatabase("test")[collName].insert({_id: "after_abort"}));
            sessions.push(session);
        }

        abortAll(sessions);
    }

    jsTest.log("limits all session types from user connections");
    {
        assert.commandWorked(
            primary.adminCommand({setParameter: 1, maxConcurrentMultiDocumentTransactions: limit}));

        const sessions = fillToLimit(primary, collName, limit, "stype");
        const testDB = primary.getDB("test");

        {
            const res = testDB.runCommand({
                insert: collName,
                documents: [{_id: "regular_over_limit"}],
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
            });
            assert.commandFailedWithCode(res, ErrorCodes.TooManyOpenTransactions);
        }

        {
            const retryableLsid = {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
            const res = testDB.runCommand({
                insert: collName,
                documents: [{_id: "retryable_over_limit"}],
                lsid: retryableLsid,
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
            });
            assert.commandFailedWithCode(res, ErrorCodes.TooManyOpenTransactions);
        }

        {
            const nonRetryableLsid = {id: UUID(), txnUUID: UUID()};
            const res = testDB.runCommand({
                insert: collName,
                documents: [{_id: "nonretryable_over_limit"}],
                lsid: nonRetryableLsid,
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
            });
            assert.commandFailedWithCode(res, ErrorCodes.TooManyOpenTransactions);
        }

        abortAll(sessions);
    }

    jsTest.log("allows process-internal transactions to bypass the limit");
    {
        assert.commandWorked(
            primary.adminCommand({setParameter: 1, maxConcurrentMultiDocumentTransactions: limit}));

        const sessions = fillToLimit(primary, collName, limit, "itxn");

        {
            const session = primary.startSession();
            session.startTransaction();
            const res = session.getDatabase("test")[collName].insert({_id: "itxn_user_over_limit"});
            assert.commandFailedWithCode(res, ErrorCodes.TooManyOpenTransactions);
            session.endSession();
        }

        {
            const res = primary.adminCommand({
                testInternalTransactions: 1,
                commandInfos: [{
                    dbName: "test",
                    command: {insert: collName, documents: [{_id: "internal_bypass"}]},
                }],
            });
            assert.commandWorked(res);
        }

        abortAll(sessions);
    }

    jsTest.log("disables enforcement when limit is 0");
    {
        assert.commandWorked(
            primary.adminCommand({setParameter: 1, maxConcurrentMultiDocumentTransactions: 0}));

        const sessions = fillToLimit(primary, collName, 200, "unlimited");
        abortAll(sessions);

        assert.commandWorked(
            primary.adminCommand({setParameter: 1, maxConcurrentMultiDocumentTransactions: limit}));
    }

    rst.stopSet();
}

function runShardedTests() {
    jsTest.log("limits user transactions routed through mongos");
    const st = new ShardingTest({shards: 1});
    const shardPrimary = st.rs0.getPrimary();
    const mongos = st.s;
    const collName = "sharded_txn_limit";

    assert.commandWorked(mongos.getDB("test").createCollection(collName));

    const shardedLimit = 10;
    assert.commandWorked(shardPrimary.adminCommand(
        {setParameter: 1, maxConcurrentMultiDocumentTransactions: shardedLimit}));

    const sessions = fillToLimit(mongos, collName, shardedLimit, "mongos");

    {
        const session = mongos.startSession();
        session.startTransaction();
        const res = session.getDatabase("test")[collName].insert({_id: "mongos_over_limit"});
        assert.commandFailedWithCode(res, ErrorCodes.TooManyOpenTransactions);
        session.endSession();
    }

    abortAll(sessions);
    st.stop();
}

runReplicaSetTests();
runShardedTests();
})();
