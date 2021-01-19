/**
 * Tests startup recovery to tenant migration donor's in memory state. This test randomly selects a
 * point during the migration to simulate a failure.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donor',
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor, {
        setParameter:
            {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"})}
    })
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    return;
}

const kMaxSleepTimeMS = 1000;
const kTenantId = 'testTenantId';

let donorPrimary = tenantMigrationTest.getDonorPrimary();
let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);

// Force the migration to pause after entering a randomly selected state to simulate a failure.
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

try {
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
} catch (e) {
    if (isNetworkError(e)) {
        jsTestLog('Ignoring network error due to node being restarted: ' + tojson(e));
        return;
    }
    throw e;
}

sleep(Math.random() * kMaxSleepTimeMS);
donorRst.stopSet(null /* signal */, true /*forRestart */);
donorRst.startSet({
    restart: true,
    setParameter: {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": "{'mode':'alwaysOn'}"}
});

donorPrimary = donorRst.getPrimary();
configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
if (donorDoc) {
    let state = donorDoc.state;
    switch (state) {
        case TenantMigrationTest.State.kDataSync:
            assert.soon(
                () => tenantMigrationTest.getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                          .state == TenantMigrationTest.AccessState.kAllow);
            break;
        case TenantMigrationTest.State.kBlocking:
            assert.soon(
                () => tenantMigrationTest.getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                          .state == TenantMigrationTest.AccessState.kBlockWritesAndReads);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            break;
        case TenantMigrationTest.State.kCommitted:
            assert.soon(
                () => tenantMigrationTest.getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                          .state == TenantMigrationTest.AccessState.kReject);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                                        .commitOpTime,
                                    donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            assert.commandWorked(
                tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
            break;
        case TenantMigrationTest.State.kAborted:
            assert.soon(
                () => tenantMigrationTest.getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                          .state == TenantMigrationTest.AccessState.kAborted);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                                        .abortOpTime,
                                    donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(donorPrimary, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            assert.commandWorked(
                tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
            break;
        default:
            throw new Error(`Invalid state "${state}" from donor doc.`);
    }
}

tenantMigrationTest.stop();
donorRst.stopSet();
})();
