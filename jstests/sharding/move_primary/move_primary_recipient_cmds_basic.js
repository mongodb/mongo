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
const collName = 'testcoll0';

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: donor.shardName}));

const donorColl0 = assertDropAndRecreateCollection(testDB, collName);
const recipientColl0 = recipient.getDB(dbName).getCollection(collName);

assert.commandWorked(donorColl0.insert([{a: 1}, {b: 1}]));

assert.eq(2, donorColl0.find().itcount(), "Donor does not have data before move");
assert.eq(0, recipientColl0.find().itcount(), "Recipient has data before move");

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

assert.eq(2, recipientColl0.count(), "Data has not been cloned to the Recipient correctly");

// Test that _movePrimaryRecipientForgetMigration called on an already forgotten migration succeeds
assert.commandWorked(recipient.adminCommand({
    _movePrimaryRecipientForgetMigration: 1,
    migrationId: uuid,
    databaseName: dbName,
    fromShardName: donor.shardName,
    toShardName: recipient.shardName
}));

// Test that _movePrimaryRecipientAbortMigration command aborts an ongoing movePrimary op.
assertDropCollection(recipient.getDB(dbName), collName);
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

assert.eq(0, recipientColl0.count(), "Recipient has orphaned collections");

// Cleanup to prevent metadata inconsistencies as we are not committing config changes.
assert.commandWorked(recipient.getDB(dbName).dropDatabase());
assert.commandWorked(donor.getDB(dbName).dropDatabase());

st.stop();
})();
