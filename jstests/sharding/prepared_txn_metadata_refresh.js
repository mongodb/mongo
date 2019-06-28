/**
 * Test to make sure that transactions doesn't block shard version metadata refresh.
 * Test relies on the fact that destination shard does not update it's shard version after a
 * migration when doNotRefreshRecipientAfterCommit failpoint is ON.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
    "use strict";

    load('./jstests/libs/chunk_manipulation_util.js');

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    let st = new ShardingTest({shards: 3, other: {shardOptions: {verbose: 1}}});

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 0}}));
    assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: -100}}));
    assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 100}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'test.user', find: {x: 100}, to: st.shard2.shardName}));

    // Send a normal write to establish the shard versions outside the transaction.
    assert.commandWorked(st.s.getDB('test').runCommand({
        insert: 'user',
        documents: [{x: 0}, {x: 100}],
    }));

    let lsid = {id: UUID()};
    let txnNumber = 0;

    // Start a migration in parallel to get around the X lock acquisition at the beginning of
    // migration at the destination.
    assert.commandWorked(st.rs0.getPrimary().getDB('admin').runCommand(
        {configureFailPoint: 'doNotRefreshRecipientAfterCommit', mode: 'alwaysOn'}));

    let destPrimary = st.rs2.getPrimary();
    pauseMigrateAtStep(destPrimary, migrateStepNames.cloned);
    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {x: -100}, null, 'test.user', st.shard2.shardName);
    waitForMigrateStep(destPrimary, migrateStepNames.cloned);

    assert.commandWorked(st.s.getDB('test').runCommand({
        insert: 'user',
        documents: [{x: 1}, {x: 101}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));

    unpauseMigrateAtStep(destPrimary, migrateStepNames.cloned);
    joinMoveChunk();

    // Make the transaction stay in prepared state so it will hold on to the collection locks.
    assert.commandWorked(st.rs1.getPrimary().getDB('admin').runCommand(
        {configureFailPoint: 'hangBeforeWritingDecision', mode: 'alwaysOn'}));
    assert.commandWorked(st.rs2.getPrimary().getDB('admin').runCommand(
        {configureFailPoint: 'hangBeforeWritingDecision', mode: 'alwaysOn'}));

    const runCommitCode = "db.adminCommand({" + "commitTransaction: 1," + "lsid: " + tojson(lsid) +
        "," + "txnNumber: NumberLong(" + txnNumber + ")," + "stmtId: NumberInt(0)," +
        "autocommit: false," + "});";
    let commitTxn = startParallelShell(runCommitCode, st.s.port);

    // Insert should be able to refresh the sharding metadata even with existing transactions
    // holding the collection lock in IX.
    assert.commandWorked(st.s.getDB('test').runCommand(
        {insert: 'user', documents: [{x: -100}], maxTimeMS: 5 * 1000}));

    assert.commandWorked(st.rs1.getPrimary().getDB('admin').runCommand(
        {configureFailPoint: 'hangBeforeWritingDecision', mode: 'off'}));
    assert.commandWorked(st.rs2.getPrimary().getDB('admin').runCommand(
        {configureFailPoint: 'hangBeforeWritingDecision', mode: 'off'}));
    commitTxn();

    st.stop();
    MongoRunner.stopMongod(staticMongod);

})();
