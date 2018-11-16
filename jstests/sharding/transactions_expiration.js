// Tests that a transaction expires and is then aborted by the server. Uses the server parameter
// 'transactionLifetimeLimitSeconds' to lower the transaction lifetime for quicker transaction
// expiration. This is the mongos version for: jstests/core/txns/abort_expired_transaction.js.
//
// @tags: [uses_transactions, uses_multi_shard_transaction]

(function() {
    "use strict";

    let st = new ShardingTest({shards: 2});

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.name);
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.name}));

    let lowerTxnTimeout = (conn) => {
        assert.commandWorked(
            conn.getDB('admin').runCommand({setParameter: 1, transactionLifetimeLimitSeconds: 1}));
    };

    st.rs0.nodes.forEach(lowerTxnTimeout);
    st.rs1.nodes.forEach(lowerTxnTimeout);

    let testDB = st.s.getDB('test');

    // Create the collections in the shards outside the transactions.
    assert.commandWorked(testDB.runCommand(
        {insert: 'user', documents: [{x: -1}, {x: 1}], writeConcern: {w: 'majority'}}));

    const session = st.s.startSession();
    const sessionDb = session.getDatabase('test');

    let txnNumber = 0;

    assert.commandWorked(sessionDb.runCommand({
        insert: 'user',
        documents: [{x: -10}, {x: 10}],
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }));

    // We can deterministically wait for the transaction to be aborted by waiting for currentOp
    // to cease reporting the inactive transaction: the transaction should disappear from the
    // currentOp results once aborted.
    assert.soon(
        function() {
            const sessionFilter = {
                active: false,
                opid: {$exists: false},
                desc: "inactive transaction",
                "transaction.parameters.txnNumber": NumberLong(txnNumber),
                "lsid.id": session.getSessionId().id
            };

            const priConn = st.rs0.getPrimary();
            const res = priConn.getDB("admin").aggregate(
                [{$currentOp: {allUsers: true, idleSessions: true}}, {$match: sessionFilter}]);

            return (res.itcount() == 0);
        },
        "currentOp reports that the idle transaction still exists, it has not been " +
            "aborted as expected.");

    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: 'user',
        documents: [{x: -100}, {x: 100}],
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction);

    session.endSession();

    st.stop();
}());
