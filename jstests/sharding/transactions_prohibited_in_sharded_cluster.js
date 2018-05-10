/**
 * Verify that multi-statement transactions are disallowed on mongos.
 */
(function() {
    'use strict';

    const st = new ShardingTest({shards: 1, mongos: 1, config: 1, rsOptions: {nodes: 1}});
    const mongos = st.s0;
    const dbName = "test";
    const collName = "coll";

    // Start a session on the mongos.
    const session = mongos.getDB(dbName).getMongo().startSession();
    const sessionDb = session.getDatabase(dbName);
    let txnNumber = 0;

    // Unable to start a transaction via mongos with a read.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        startTransaction: true,
        autocommit: false
    }),
                                 50841);

    // Unable to start a transaction via mongos with a write.
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: 1}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        startTransaction: true,
        autocommit: false
    }),
                                 50841);

    // 'startTransaction' and 'autocommit' arguments are always rejected by mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        autocommit: false
    }),
                                 50841);

    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        startTransaction: true
    }),
                                 50841);

    st.stop();

})();
