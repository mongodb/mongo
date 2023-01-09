/**
 * Tests that tenant migration donor's in memory state is recovered correctly on startup. This test
 * randomly selects a point during the migration to shutdown the donor.
 *
 * Incompatible with shard merge, which can't handle restart.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_fcv_62,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeX509OptionsForTest} from "jstests/replsets/libs/tenant_migration_util.js";
import {
    getServerlessOperationLock,
    ServerlessLockType
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donor',
    nodeOptions: Object.assign(makeX509OptionsForTest().donor, {
        setParameter:
            // In order to deterministically validate that in-memory state is preserved during
            // recovery, this failpoint prevents active migrations from continuing on process
            // restart.
            {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"})}
    })
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});

const kMaxSleepTimeMS = 1000;
const kTenantId = 'testTenantId';

let donorPrimary = tenantMigrationTest.getDonorPrimary();

// Force the migration to pause after entering a randomly selected state.
Random.setRandomSeed();
const kMigrationFpNames = [
    "pauseTenantMigrationBeforeLeavingDataSyncState",
    "pauseTenantMigrationBeforeLeavingBlockingState",
    "abortTenantMigrationBeforeLeavingBlockingState"
];
const index = Random.randInt(kMigrationFpNames.length + 1);
if (index < kMigrationFpNames.length) {
    configureFailPoint(donorPrimary, kMigrationFpNames[index]);
}

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: kTenantId,
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
sleep(Math.random() * kMaxSleepTimeMS);

donorRst.stopSet(null /* signal */, true /*forRestart */);
donorRst.startSet({
    restart: true,
    setParameter: {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": "{'mode':'alwaysOn'}"}
});

donorPrimary = donorRst.getPrimary();
const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
assert.lte(configDonorsColl.count(), 1);
const donorDoc = configDonorsColl.findOne();
if (donorDoc) {
    switch (donorDoc.state) {
        case TenantMigrationTest.DonorState.kAbortingIndexBuilds:
        case TenantMigrationTest.DonorState.kDataSync:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: donorPrimary, tenantId: kTenantId})
                                  .donor.state == TenantMigrationTest.DonorAccessState.kAllow);
            break;
        case TenantMigrationTest.DonorState.kBlocking:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: donorPrimary, tenantId: kTenantId})
                                  .donor.state ==
                            TenantMigrationTest.DonorAccessState.kBlockWritesAndReads);
            assert.soon(() => bsonWoCompare(tenantMigrationTest
                                                .getTenantMigrationAccessBlocker(
                                                    {donorNode: donorPrimary, tenantId: kTenantId})
                                                .donor.blockTimestamp,
                                            donorDoc.blockTimestamp) == 0);
            break;
        case TenantMigrationTest.DonorState.kCommitted:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: donorPrimary, tenantId: kTenantId})
                                  .donor.state == TenantMigrationTest.DonorAccessState.kReject);
            assert.soon(() => bsonWoCompare(tenantMigrationTest
                                                .getTenantMigrationAccessBlocker(
                                                    {donorNode: donorPrimary, tenantId: kTenantId})
                                                .donor.commitOpTime,
                                            donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(() => bsonWoCompare(tenantMigrationTest
                                                .getTenantMigrationAccessBlocker(
                                                    {donorNode: donorPrimary, tenantId: kTenantId})
                                                .donor.blockTimestamp,
                                            donorDoc.blockTimestamp) == 0);
            // Don't run donorForgetMigration because the fail point above prevents instances from
            // being rebuilt on step up.
            break;
        case TenantMigrationTest.DonorState.kAborted:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: donorPrimary, tenantId: kTenantId})
                                  .donor.state == TenantMigrationTest.DonorAccessState.kAborted);
            assert.soon(() => bsonWoCompare(tenantMigrationTest
                                                .getTenantMigrationAccessBlocker(
                                                    {donorNode: donorPrimary, tenantId: kTenantId})
                                                .donor.abortOpTime,
                                            donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(() => bsonWoCompare(tenantMigrationTest
                                                .getTenantMigrationAccessBlocker(
                                                    {donorNode: donorPrimary, tenantId: kTenantId})
                                                .donor.blockTimestamp,
                                            donorDoc.blockTimestamp) == 0);
            // Don't run donorForgetMigration because the fail point above prevents instances from
            // being rebuilt on step up.
            break;
        default:
            throw new Error(`Invalid state "${state}" from donor doc.`);
    }
}

const activeServerlessLock = getServerlessOperationLock(donorPrimary);
if (donorDoc && !donorDoc.expireAt) {
    assert.eq(activeServerlessLock, ServerlessLockType.TenantMigrationDonor);
} else {
    assert.eq(activeServerlessLock, ServerlessLockType.None);
}

tenantMigrationTest.stop();
donorRst.stopSet();
