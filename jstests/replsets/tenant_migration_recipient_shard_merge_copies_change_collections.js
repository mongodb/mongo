/**
 * Tests that recipient is able to copy and apply change collection entries from the donor for the
 * shard merge protocol.
 *
 * TODO SERVER-72828: remove this test from 'exclude_files' in 'replica_sets_large_txns_format.yml'
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isShardMergeEnabled,
    makeX509OptionsForTest
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/serverless/libs/change_collection_util.js");

const donorRst = new ChangeStreamMultitenantReplicaSetTest({
    name: "donorReplSet",
    nodes: 2,
    nodeOptions: Object.assign(makeX509OptionsForTest().donor, {
        setParameter: {
            tenantMigrationGarbageCollectionDelayMS: 0,
            ttlMonitorSleepSecs: 1,
        }
    }),
});
const recipientRst = new ChangeStreamMultitenantReplicaSetTest({
    name: "recipientReplSet",
    nodes: 2,
    nodeOptions: Object.assign(makeX509OptionsForTest().recipient, {
        setParameter: {
            tenantMigrationGarbageCollectionDelayMS: 0,
            ttlMonitorSleepSecs: 1,
        }
    }),
});

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    donorRst,
    recipientRst,
    quickGarbageCollection: true,
});

function assertChangeCollectionEntries(donorEntries, recipientEntries) {
    assert.eq(donorEntries.length, recipientEntries.length);
    donorEntries.forEach((donorEntry, idx) => {
        assert.eq(donorEntry, recipientEntries[idx]);
    });
}

function getChangeCollectionDocuments(conn) {
    // Filter out change collection entries for admin.system.users because 'getTenantConnection'
    // will create a user on the donor before we have enabled change streams. Also filter out
    // 'create' entries for system.change_collection, since the recipient will have an extra
    // entry for the case where changestreams are enabled for a tenant during oplog catchup.
    return conn.getDB("config")["system.change_collection"]
        .find({ns: {$ne: "admin.system.users"}})
        .toArray();
}

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!isShardMergeEnabled(tenantMigrationTest.getRecipientPrimary().getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    quit();
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId1 = ObjectId();
const tenantId2 = ObjectId();

const donorTenantConn1 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    donorPrimary.host, tenantId1, tenantId1.str);

const donorTenantConn2 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    donorPrimary.host, tenantId2, tenantId2.str);

donorRst.setChangeStreamState(donorTenantConn1, true);

// Open a change stream and insert documents into database.collection before the migration
// starts.
const donorCursor1 = donorTenantConn1.getDB("database").collection.watch([]);
donorTenantConn1.getDB("database")
    .collection.insertMany([{_id: "tenant1_1"}, {_id: "tenant1_2"}, {_id: "tenant1_3"}]);

// Get the first entry from the tenant1 change stream cursor and grab the resume token.
assert.soon(() => donorCursor1.hasNext());
const {_id: resumeToken1} = donorCursor1.next();

// Start a transaction and perform some writes.
const donorSession1 = donorTenantConn1.getDB("database").getMongo().startSession();
donorSession1.startTransaction();
donorSession1.getDatabase("database").collection.insertOne({_id: "tenant1_in_transaction_1"});
donorSession1.getDatabase("database").collection.updateOne({_id: "tenant1_in_transaction_1"}, {
    $set: {updated: true}
});
donorSession1.commitTransaction_forTesting();

const fpBeforeMarkingCloneSuccess =
    configureFailPoint(recipientPrimary, "fpBeforeMarkingCloneSuccess", {action: "hang"});

const migrationUuid = UUID();
const tenantIds = [tenantId1, tenantId2];
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: "primary"},
    tenantIds,
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

fpBeforeMarkingCloneSuccess.wait();

// Insert more documents after cloning has completed so that oplog entries are applied during oplog
// catchup.
donorTenantConn1.getDB("database").collection.insertOne({_id: "tenant1_4"});

// Enable change streams for the second tenant during oplog catchup.
donorRst.setChangeStreamState(donorTenantConn2, true);
const donorCursor2 = donorTenantConn2.getDB("database").collection.watch([]);
donorTenantConn2.getDB("database").collection.insertOne({_id: "tenant2_1"});

// Get the first entry from the tenant2 change stream cursor and grab the resume token.
assert.soon(() => donorCursor2.hasNext());
const {_id: resumeToken2} = donorCursor2.next();

// Insert another entry so that we can consume it on the Recipient after the migration has
// completed.
donorTenantConn2.getDB("database").collection.insertOne({_id: "tenant2_2"});

// Start a transaction and perform some writes.
const donorSession2 = donorTenantConn2.getDB("database").getMongo().startSession();
donorSession2.startTransaction();
donorSession2.getDatabase("database").collection.insertOne({_id: "tenant2_in_transaction_1"});
donorSession2.getDatabase("database").collection.updateOne({_id: "tenant2_in_transaction_1"}, {
    $set: {updated: true}
});
donorSession2.commitTransaction_forTesting();

fpBeforeMarkingCloneSuccess.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

tenantIds.forEach(tenantId => {
    const donorTenantConn = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        donorPrimary.host, tenantId, tenantId.str);
    const donorChangeCollectionDocuments = getChangeCollectionDocuments(donorTenantConn);

    recipientRst.nodes.forEach(recipientNode => {
        jsTestLog(
            `Performing change collection validation for tenant ${tenantId} on ${recipientNode}`);
        const recipientTenantConn = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
            recipientPrimary.host, tenantId, tenantId.str);

        assertChangeCollectionEntries(donorChangeCollectionDocuments,
                                      getChangeCollectionDocuments(recipientTenantConn));
    });
});

const recipientTenantConn1 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    recipientPrimary.host, tenantId1, tenantId1.str);

const recipientTenantConn2 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    recipientPrimary.host, tenantId2, tenantId2.str);

// Resume the first change stream on the Recipient.
const recipientCursor1 =
    recipientTenantConn1.getDB("database").collection.watch([], {resumeAfter: resumeToken1});
[{_id: "tenant1_2", operationType: "insert"},
 {_id: "tenant1_3", operationType: "insert"},
 {_id: "tenant1_in_transaction_1", operationType: "insert"},
 {_id: "tenant1_in_transaction_1", operationType: "update"},
 {_id: "tenant1_4", operationType: "insert"},
].forEach(expectedEvent => {
    assert.soon(() => recipientCursor1.hasNext());
    const changeEvent = recipientCursor1.next();
    assert.eq(changeEvent.documentKey._id, expectedEvent._id);
    assert.eq(changeEvent.operationType, expectedEvent.operationType);
});

// Resume the second change stream on the Recipient.
const recipientCursor2 =
    recipientTenantConn2.getDB("database").collection.watch([], {resumeAfter: resumeToken2});
[{_id: "tenant2_2", operationType: "insert"},
 {_id: "tenant2_in_transaction_1", operationType: "insert"},
 {_id: "tenant2_in_transaction_1", operationType: "update"},
].forEach(expectedEvent => {
    assert.soon(() => recipientCursor2.hasNext());
    const changeEvent = recipientCursor2.next();
    assert.eq(changeEvent.documentKey._id, expectedEvent._id);
    assert.eq(changeEvent.operationType, expectedEvent.operationType);
});

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);

donorRst.stopSet();
recipientRst.stopSet();
tenantMigrationTest.stop();
