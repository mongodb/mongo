/**
 * Tests that tenant migration recipient's in memory state is recovered correctly on startup. This
 * test randomly selects a point during the migration to shutdown the recipient.
 *
 *  TODO SERVER-72209 Remove incompatible_with_shard_merge once recipient is resistent to restarts
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeX509OptionsForTest} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

const recipientRst = new ReplSetTest({
    nodes: 1,
    name: 'recipient',
    nodeOptions: Object.assign(makeX509OptionsForTest().recipient, {
        setParameter:
            {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"})}
    })
});

recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});

const kMaxSleepTimeMS = 7500;
const kTenantId = ObjectId().str;

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

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
sleep(Math.random() * kMaxSleepTimeMS);

recipientRst.stopSet(null /* signal */, true /*forRestart */);
recipientRst.startSet({
    restart: true,
    setParameter: {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": "{'mode':'alwaysOn'}"}
});

recipientPrimary = recipientRst.getPrimary();
const configRecipientsColl =
    recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS);
const recipientDoc = configRecipientsColl.findOne({_id: migrationId});
if (recipientDoc) {
    switch (recipientDoc.state) {
        case TenantMigrationTest.RecipientState.kStarted:
            if (recipientDoc.dataConsistentStopDonorOpTime) {
                assert.soon(() => tenantMigrationTest
                                      .getTenantMigrationAccessBlocker(
                                          {recipientNode: recipientPrimary, tenantId: kTenantId})
                                      .recipient.state ==
                                TenantMigrationTest.RecipientAccessState.kReject);
            }
            break;
        case TenantMigrationTest.RecipientState.kConsistent:
            if (recipientDoc.rejectReadsBeforeTimestamp) {
                assert.soon(() => tenantMigrationTest
                                      .getTenantMigrationAccessBlocker(
                                          {recipientNode: recipientPrimary, tenantId: kTenantId})
                                      .recipient.state ==
                                TenantMigrationTest.RecipientAccessState.kRejectBefore);
                assert.soon(() =>
                                bsonWoCompare(
                                    tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(
                                            {recipientNode: recipientPrimary, tenantId: kTenantId})
                                        .recipient.rejectBeforeTimestamp,
                                    recipientDoc.rejectReadsBeforeTimestamp) == 0);
            } else {
                assert.soon(() => tenantMigrationTest
                                      .getTenantMigrationAccessBlocker(
                                          {recipientNode: recipientPrimary, tenantId: kTenantId})
                                      .recipient.state ==
                                TenantMigrationTest.RecipientAccessState.kReject);
            }
            break;
        default:
            throw new Error(`Invalid state "${state}" from recipient doc.`);
    }
}

tenantMigrationTest.stop();
recipientRst.stopSet();
