/**
 * Tests that MovePrimaryRecipient commands work as intended.
 *
 * @tags: [featureFlagOnlineMovePrimaryLifecycle]
 */
(function() {
'use strict';

load("jstests/libs/collection_drop_recreate.js");

const st = new ShardingTest({mongos: 1, shards: 2});

const mongos = st.s0;
const donor = st.shard0;
const recipient = st.shard1;

const dbName = jsTestName();
const testDB = donor.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: donor.shardName}));

const coll0 = assertDropAndRecreateCollection(testDB, 'testcoll0');
assert.commandWorked(coll0.insert([{a: 1}, {b: 1}]));

function runRecipientSyncDataCmds(uuid) {
    assert.commandWorked(recipient.adminCommand({
        _movePrimaryRecipientSyncData: 1,
        migrationId: uuid,
        databaseName: dbName,
        fromShardName: donor.shardName,
        toShardName: recipient.shardName
    }));

    assert.commandWorked(recipient.adminCommand({
        _movePrimaryRecipientSyncData: 1,
        returnAfterReachingDonorTimestamp: new Timestamp(1, 1),
        migrationId: uuid,
        databaseName: dbName,
        fromShardName: donor.shardName,
        toShardName: recipient.shardName
    }));
}

// Test that _movePrimaryRecipientSyncData commands work followed by
// _movePrimaryRecipientForgetMigration.
let uuid = UUID();

runRecipientSyncDataCmds(uuid);

assert.commandWorked(recipient.adminCommand({
    _movePrimaryRecipientForgetMigration: 1,
    migrationId: uuid,
    databaseName: dbName,
    fromShardName: donor.shardName,
    toShardName: recipient.shardName
}));

// Test that _movePrimaryRecipientForgetMigration called on an already forgotten migration succeeds
assert.commandWorked(recipient.adminCommand({
    _movePrimaryRecipientForgetMigration: 1,
    migrationId: uuid,
    databaseName: dbName,
    fromShardName: donor.shardName,
    toShardName: recipient.shardName
}));

// Test that _movePrimaryRecipientAbortMigration command aborts an ongoing movePrimary op.
uuid = UUID();

runRecipientSyncDataCmds(uuid);

assert.commandWorked(recipient.adminCommand({
    _movePrimaryRecipientAbortMigration: 1,
    migrationId: uuid,
    databaseName: dbName,
    fromShardName: donor.shardName,
    toShardName: recipient.shardName
}));

// Test that _movePrimaryRecipientAbortMigration called on an already aborted migration succeeds
assert.commandWorked(recipient.adminCommand({
    _movePrimaryRecipientAbortMigration: 1,
    migrationId: uuid,
    databaseName: dbName,
    fromShardName: donor.shardName,
    toShardName: recipient.shardName
}));

// TODO SERVER-74707: Verify documents are cloned after integrating offline cloner.

st.stop();
})();
