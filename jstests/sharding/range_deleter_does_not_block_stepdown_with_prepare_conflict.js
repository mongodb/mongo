/**
 * Test to make sure that range deleter deletions blocked on prepare conflicts do not prevent
 * stepdown from occuring.
 *
 * Layout:
 * 1. Shard a collection and split it into two chunks, both of which live on shard 0.
 * 2. Pause range deletion.
 * 3. Move one chunk from shard 0 to shard 1.
 * 4. In a transaction, insert a document into the chunk that remains on shard 0.
 * 5. Put the transaction into prepare.
 * 6. Resume the range deleter. The range deletion task should block behind the prepared
 * transaction.
 * 7. Attempt a step down. This step down should kill the range deleter operation and succeed.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */
(function() {
    "use strict";

    load('jstests/sharding/libs/sharded_transactions_helpers.js');

    TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

    // Helper to add generic txn fields to a command.
    function addTxnFieldsToCmd(cmd, lsid, txnNumber) {
        return Object.extend(
            cmd, {lsid, txnNumber: NumberLong(txnNumber), stmtId: NumberInt(0), autocommit: false});
    }

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    const st = new ShardingTest({shards: [{verbose: 1}, {verbose: 1}], config: 1});

    // Set up sharded collection with two chunks - [-inf, 0), [0, inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

    st.rs0.getPrimary().adminCommand(
        {configureFailPoint: 'suspendRangeDeletion', mode: 'alwaysOn'});
    // Move a chunk away from Shard0 (the donor) so its range deleter will asynchronously delete the
    // chunk's range. Flush its metadata to avoid StaleConfig during the later transaction.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: st.shard1.shardName}));
    assert.commandWorked(st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));

    // Insert a doc into the chunk still owned by the donor shard in a transaction then prepare the
    // transaction so readers of that doc will enter a prepare conflict retry loop.
    const lsid = {id: UUID()};
    const txnNumber = 0;
    assert.commandWorked(st.s.getDB(dbName).runCommand(addTxnFieldsToCmd(
        {insert: collName, documents: [{_id: -5}], startTransaction: true}, lsid, txnNumber)));

    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        addTxnFieldsToCmd({prepareTransaction: 1}, lsid, txnNumber)));

    // Set a failpoint to hang right after beginning the index scan for documents to delete.
    st.rs0.getPrimary().adminCommand(
        {configureFailPoint: 'hangBeforeDoingDeletion', mode: 'alwaysOn'});

    // Allow the range deleter to run. It should get stuck in a prepare conflict retry loop.
    st.rs0.getPrimary().adminCommand({configureFailPoint: 'suspendRangeDeletion', mode: 'off'});

    // Wait until we've started the index scan to delete documents.
    waitForFailpoint("Hit hangBeforeDoingDeletion failpoint", 1);

    // Let the deletion continue.
    st.rs0.getPrimary().adminCommand({configureFailPoint: 'hangBeforeDoingDeletion', mode: 'off'});

    // Attempt to step down the primary.
    assert.commandWorked(st.rs0.getPrimary().adminCommand({replSetStepDown: 5, force: true}));

    // Cleanup the transaction so the sharding test can shut down.
    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        addTxnFieldsToCmd({abortTransaction: 1}, lsid, txnNumber)));

    st.stop();
})();
