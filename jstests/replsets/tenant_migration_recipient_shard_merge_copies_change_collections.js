/**
 * Tests that recipient is able to copy and apply change collection entries from the donor for the
 * shard merge protocol.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_fcv_70,
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
donorTenantConn1.getDB("database").collection.updateOne({_id: "tenant1_3"}, {
    $set: {updated: true}
});

// Get the first entry from the tenant1 change stream cursor and grab the resume token.
assert.soon(() => donorCursor1.hasNext());
const {_id: resumeToken1} = donorCursor1.next();

const donorTenant1Session = donorTenantConn1.startSession({retryWrites: true});
const donorTenant1SessionCollection = donorTenant1Session.getDatabase("database").collection;
assert.commandWorked(donorTenant1SessionCollection.insert({_id: "tenant1_4", w: "RETRYABLE"}));
assert.commandWorked(donorTenant1Session.getDatabase("database").runCommand({
    findAndModify: "collection",
    query: {_id: "tenant1_4"},
    update: {$set: {updated: true}}
}));

// Start a transaction and perform some writes.
const donorTxnSession1 = donorTenantConn1.getDB("database").getMongo().startSession();
donorTxnSession1.startTransaction();
donorTxnSession1.getDatabase("database").collection.insertOne({_id: "tenant1_in_transaction_1"});
donorTxnSession1.getDatabase("database").collection.updateOne({_id: "tenant1_in_transaction_1"}, {
    $set: {updated: true}
});
donorTxnSession1.commitTransaction();
donorTxnSession1.endSession();

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
assert.commandWorked(donorTenantConn1.getDB("database").collection.updateOne({_id: "tenant1_2"}, {
    $set: {updated: true}
}));

assert.commandWorked(donorTenant1SessionCollection.insert({_id: "tenant1_5", w: "RETRYABLE"}));
assert.commandWorked(
    donorTenant1SessionCollection.updateOne({_id: "tenant1_5"}, {$set: {updated: true}}));

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
donorSession2.commitTransaction();

// Start a transaction and perform some large writes.
const largePad = "a".repeat(10 * 1024 * 1024);
donorSession2.startTransaction();
donorSession2.getDatabase("database")
    .collection.insertOne({_id: "tenant2_in_transaction_2", largePad});
donorSession2.getDatabase("database").collection.updateOne({_id: "tenant2_in_transaction_2"}, {
    $set: {updated: true, largePad: "b" + largePad}
});
donorSession2.commitTransaction_forTesting();

fpBeforeMarkingCloneSuccess.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationUuid, tenantIds[0]);

const recipientPrimaryTenantConn1 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    recipientPrimary.host, tenantId1, tenantId1.str);

// Running ChangeStreamMultitenantReplicaSetTest.getTenantConnection will create a user on the
// primary. Await replication so that we can use the same user on secondaries.
recipientRst.awaitReplication();

tenantIds.forEach(tenantId => {
    const donorTenantConn = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        donorPrimary.host, tenantId, tenantId.str);
    const donorChangeCollectionDocuments = getChangeCollectionDocuments(donorTenantConn);

    recipientRst.nodes.forEach(recipientNode => {
        jsTestLog(
            `Performing change collection validation for tenant ${tenantId} on ${recipientNode}`);
        const recipientTenantConn = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
            recipientNode.host, tenantId, tenantId.str);

        assertChangeCollectionEntries(donorChangeCollectionDocuments,
                                      getChangeCollectionDocuments(recipientTenantConn));
    });
});

const recipientSecondaryTenantConn1 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    recipientRst.getSecondary().host, tenantId1, tenantId1.str);

// Resume the first change stream on the Recipient primary.
const recipientPrimaryCursor1 =
    recipientPrimaryTenantConn1.getDB("database").collection.watch([], {resumeAfter: resumeToken1});

// Resume the first change stream on the Recipient secondary.
const recipientSecondaryCursor1 =
    recipientSecondaryTenantConn1.getDB("database").collection.watch([], {
        resumeAfter: resumeToken1,
    });

[{_id: "tenant1_2", operationType: "insert"},
 {_id: "tenant1_3", operationType: "insert"},
 {_id: "tenant1_3", operationType: "update"},
 {_id: "tenant1_4", operationType: "insert"},
 {_id: "tenant1_4", operationType: "update"},
 {_id: "tenant1_in_transaction_1", operationType: "insert"},
 {_id: "tenant1_in_transaction_1", operationType: "update"},
 {_id: "tenant1_2", operationType: "update"},
 {_id: "tenant1_5", operationType: "insert"},
 {_id: "tenant1_5", operationType: "update"},
].forEach(expectedEvent => {
    [recipientPrimaryCursor1, recipientSecondaryCursor1].forEach(cursor => {
        assert.soon(() => cursor.hasNext());
        const changeEvent = cursor.next();
        assert.eq(changeEvent.documentKey._id, expectedEvent._id);
        assert.eq(changeEvent.operationType, expectedEvent.operationType);
    });
});

const recipientPrimaryTenantConn2 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    recipientPrimary.host, tenantId2, tenantId2.str);

// Running ChangeStreamMultitenantReplicaSetTest.getTenantConnection will create a user on the
// primary. Await replication so that we can use the same user on secondaries.
recipientRst.awaitReplication();

const recipientSecondaryTenantConn2 = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    recipientRst.getSecondary().host, tenantId2, tenantId2.str);

// Resume the second change stream on the Recipient primary.
const recipientPrimaryCursor2 =
    recipientPrimaryTenantConn2.getDB("database").collection.watch([{$unset: "largePad"}], {
        resumeAfter: resumeToken2
    });

// Resume the second change stream on the Recipient secondary.
const recipientSecondaryCursor2 =
    recipientSecondaryTenantConn2.getDB("database").collection.watch([{$unset: "largePad"}], {
        resumeAfter: resumeToken2,
    });

[{_id: "tenant2_2", operationType: "insert"},
 {_id: "tenant2_in_transaction_1", operationType: "insert"},
 {_id: "tenant2_in_transaction_1", operationType: "update"},
 {_id: "tenant2_in_transaction_2", operationType: "insert"},
 {_id: "tenant2_in_transaction_2", operationType: "update"},
].forEach(expectedEvent => {
    [recipientPrimaryCursor2, recipientSecondaryCursor2].forEach(cursor => {
        assert.soon(() => cursor.hasNext());
        const changeEvent = cursor.next();
        assert.eq(changeEvent.documentKey._id, expectedEvent._id);
        assert.eq(changeEvent.operationType, expectedEvent.operationType);
    });
});

donorRst.stopSet();
recipientRst.stopSet();
tenantMigrationTest.stop();
