/**
 * Tests that tenant migration recipient's in memory state is initialized correctly on initial sync.
 * This test randomly selects a point during the migration to add a node to the recipient.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kMaxSleepTimeMS = 7500;
const kTenantId = 'testTenantId';

let recipientPrimary = tenantMigrationTest.getRecipientPrimary();

// Force the migration to pause after entering a randomly selected state.
Random.setRandomSeed();
const kMigrationFpNames = [
    "fpBeforeFetchingCommittedTransactions",
    "fpAfterWaitForRejectReadsBeforeTimestamp",
];
const index = Random.randInt(kMigrationFpNames.length + 1);
if (index < kMigrationFpNames.length) {
    configureFailPoint(recipientPrimary, kMigrationFpNames[index], {action: "hang"});
}

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: kTenantId
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
sleep(Math.random() * kMaxSleepTimeMS);

// Add the initial sync node and make sure that it does not step up.
const recipientRst = tenantMigrationTest.getRecipientRst();
const initialSyncNode = recipientRst.add({rsConfig: {priority: 0, votes: 0}});

recipientRst.reInitiate();
jsTestLog("Waiting for initial sync to finish.");
recipientRst.awaitSecondaryNodes();
recipientRst.awaitReplication();

// Stop replication on the node so that the TenantMigrationAccessBlocker cannot transition its state
// past what is reflected in the state doc read below.
stopServerReplication(initialSyncNode);

const configRecipientsColl = initialSyncNode.getCollection(TenantMigrationTest.kConfigRecipientsNS);
const recipientDoc = configRecipientsColl.findOne({tenantId: kTenantId});
if (recipientDoc) {
    switch (recipientDoc.state) {
        case TenantMigrationTest.RecipientState.kStarted:
            if (recipientDoc.dataConsistentStopDonorOpTime) {
                assert.soon(() => tenantMigrationTest
                                      .getTenantMigrationAccessBlocker(
                                          {recipientNode: initialSyncNode, tenantId: kTenantId})
                                      .recipient.state ==
                                TenantMigrationTest.RecipientAccessState.kReject);
            }
            break;
        case TenantMigrationTest.RecipientState.kConsistent:
            if (recipientDoc.rejectReadsBeforeTimestamp) {
                assert.soon(() => tenantMigrationTest
                                      .getTenantMigrationAccessBlocker(
                                          {recipientNode: initialSyncNode, tenantId: kTenantId})
                                      .recipient.state ==
                                TenantMigrationTest.RecipientAccessState.kRejectBefore);
                assert.soon(() => bsonWoCompare(
                                      tenantMigrationTest
                                          .getTenantMigrationAccessBlocker(
                                              {recipientNode: initialSyncNode, tenantId: kTenantId})
                                          .recipient.rejectBeforeTimestamp,
                                      recipientDoc.rejectReadsBeforeTimestamp) == 0);
            } else {
                assert.soon(() => tenantMigrationTest
                                      .getTenantMigrationAccessBlocker(
                                          {recipientNode: initialSyncNode, tenantId: kTenantId})
                                      .recipient.state ==
                                TenantMigrationTest.RecipientAccessState.kReject);
            }
            break;
        default:
            throw new Error(`Invalid state "${state}" from recipient doc.`);
    }
}

restartServerReplication(initialSyncNode);

tenantMigrationTest.stop();
})();
