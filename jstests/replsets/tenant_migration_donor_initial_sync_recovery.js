/**
 * Tests initial sync's recovery to a tenant migration's in-memory state.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const donorRst = new ReplSetTest(
    {nodes: 1, name: 'donor', nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const recipientRst = new ReplSetTest({
    nodes: 1,
    name: 'recipient',
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,
            // TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
        }
    }
});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const kMaxSleepTimeMS = 1000;
const kTenantId = 'testTenantId';
const kConfigDonorsNS = "config.tenantMigrationDonors";

let donorPrimary = donorRst.getPrimary();
let kRecipientConnString = recipientRst.getURL();

function startMigration(host, recipientConnString, tenantId) {
    const primary = new Mongo(host);
    assert.commandWorked(primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: recipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"}
    }));
}

let migrationThread =
    new Thread(startMigration, donorPrimary.host, kRecipientConnString, kTenantId);

// Force the migration to pause after entering a randomly selected state to simulate a failure.
Random.setRandomSeed();
const kMigrationFpNames = [
    "pauseTenantMigrationAfterDataSync",
    "pauseTenantMigrationAfterBlockingStarts",
    "abortTenantMigrationAfterBlockingStarts"
];
const index = Random.randInt(kMigrationFpNames.length + 1);
if (index < kMigrationFpNames.length) {
    configureFailPoint(donorPrimary, kMigrationFpNames[index]);
}

migrationThread.start();
sleep(Math.random() * kMaxSleepTimeMS);

// Add the initial sync node and make sure that it does not step up.
var initialSyncNode =
    donorRst.add({rsConfig: {priority: 0, votes: 0}, setParameter: {enableTenantMigrations: true}});

donorRst.reInitiate();
jsTestLog("Waiting for initial sync to finish.");
donorRst.awaitSecondaryNodes();

let configDonorsColl = initialSyncNode.getCollection(kConfigDonorsNS);
let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
if (donorDoc) {
    let state = donorDoc.state;
    switch (state) {
        case "data sync":
            assert.soon(() => TenantMigrationUtil
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationUtil.accessState.kAllow);
            break;
        case "blocking":
            assert.soon(() => TenantMigrationUtil
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationUtil.accessState.kBlockWritesAndReads);
            assert.soon(
                () => bsonWoCompare(TenantMigrationUtil
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            break;
        case "committed":
            assert.soon(() => TenantMigrationUtil
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationUtil.accessState.kReject);
            assert.soon(
                () => bsonWoCompare(TenantMigrationUtil
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .commitOrAbortOpTime,
                                    donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(TenantMigrationUtil
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            break;
        case "aborted":
            assert.soon(() => TenantMigrationUtil
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationUtil.accessState.kAborted);
            assert.soon(
                () => bsonWoCompare(TenantMigrationUtil
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .commitOrAbortOpTime,
                                    donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(TenantMigrationUtil
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            break;
        default:
            throw new Error(`Invalid state "${state}" from donor doc.`);
    }
}

migrationThread.join();
donorRst.stopSet();
recipientRst.stopSet();
})();
