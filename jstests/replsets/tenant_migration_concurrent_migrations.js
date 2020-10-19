/**
 * Test that multiple concurrent tenant migrations are supported.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

let startupParams = {};
startupParams["enableTenantMigrations"] = true;
// TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
startupParams["failpoint.returnResponseOkForRecipientSyncDataCmd"] = tojson({mode: 'alwaysOn'});

const rst0 = new ReplSetTest({nodes: 1, name: 'rst0', nodeOptions: {setParameter: startupParams}});
const rst1 = new ReplSetTest({nodes: 1, name: 'rst1', nodeOptions: {setParameter: startupParams}});
const rst2 = new ReplSetTest({nodes: 1, name: 'rst2', nodeOptions: {setParameter: startupParams}});

rst0.startSet();
rst0.initiate();

rst1.startSet();
rst1.initiate();

rst2.startSet();
rst2.initiate();

const rst0Primary = rst0.getPrimary();
const rst1Primary = rst1.getPrimary();

const kConfigDonorsNS = "config.tenantMigrationDonors";
const kTenantId = "testTenantId";

// Test concurrent outgoing migrations to different recipients.
(() => {
    const tenantId = kTenantId + "ConcurrentOutgoingMigrationsToDifferentRecipient";
    const donorsColl = rst0Primary.getCollection(kConfigDonorsNS);

    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst1.getURL(),
        tenantId: tenantId + "0",
        readPreference: {mode: "primary"}
    };
    const migrationOpts1 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst2.getURL(),
        tenantId: tenantId + "1",
        readPreference: {mode: "primary"}
    };

    let migrationThread0 =
        new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts0);
    let migrationThread1 =
        new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts1);

    migrationThread0.start();
    migrationThread1.start();
    migrationThread0.join();
    migrationThread1.join();

    // Verify that both migrations succeeded.
    assert.commandWorked(migrationThread0.returnData());
    assert.commandWorked(migrationThread1.returnData());
    assert(donorsColl.findOne({tenantId: migrationOpts0.tenantId, state: "committed"}));
    assert(donorsColl.findOne({tenantId: migrationOpts1.tenantId, state: "committed"}));
})();

// Test concurrent incoming migrations from different donors.
(() => {
    const tenantId = kTenantId + "ConcurrentIncomingMigrations";
    const donorsColl0 = rst0Primary.getCollection(kConfigDonorsNS);
    const donorsColl1 = rst1Primary.getCollection(kConfigDonorsNS);

    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst2.getURL(),
        tenantId: tenantId + "0",
        readPreference: {mode: "primary"}
    };
    const migrationOpts1 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst2.getURL(),
        tenantId: tenantId + "1",
        readPreference: {mode: "primary"}
    };

    let migrationThread0 =
        new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts0);
    let migrationThread1 =
        new Thread(TenantMigrationUtil.startMigration, rst1Primary.host, migrationOpts1);

    migrationThread0.start();
    migrationThread1.start();
    migrationThread0.join();
    migrationThread1.join();

    // Verify that both migrations succeeded.
    assert.commandWorked(migrationThread0.returnData());
    assert.commandWorked(migrationThread1.returnData());
    assert(donorsColl0.findOne({tenantId: migrationOpts0.tenantId, state: "committed"}));
    assert(donorsColl1.findOne({tenantId: migrationOpts1.tenantId, state: "committed"}));
})();

// TODO (SERVER-50467): Ensure that tenant migration donor only removes a ReplicaSetMonitor for
// a recipient when the last migration to that recipient completes. Before SERVER-50467, one of the
// migration thread could try to remove the recipient RSM while the other is still using it.
// Test concurrent outgoing migrations to same recipient.
// (() => {
//     const tenantId = kTenantId + "ConcurrentOutgoingMigrationsToSameRecipient";
//     const donorsColl = rst0Primary.getCollection(kConfigDonorsNS);

//     const migrationOpts0 = {
//         migrationIdString: extractUUIDFromObject(UUID()),
//         recipientConnString: rst1.getURL(),
//         tenantId: tenantId + "0",
//         readPreference: {mode: "primary"}
//     };
//     const migrationOpts1 = {
//         migrationIdString: extractUUIDFromObject(UUID()),
//         recipientConnString: rst1.getURL(),
//         tenantId: tenantId + "1",
//         readPreference: {mode: "primary"}
//     };

//     const connPoolStatsBefore = assert.commandWorked(rst0Primary.adminCommand({connPoolStats:
//     1})); assert.eq(Object.keys(connPoolStatsBefore.replicaSets).length, 0);

//     let migrationThread0 =
//         new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts0);
//     let migrationThread1 =
//         new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts1);
//     let blockFp = configureFailPoint(rst0Primary, "pauseTenantMigrationAfterBlockingStarts");

//     // Make sure that there is an overlap between the two migrations.
//     migrationThread0.start();
//     migrationThread1.start();
//     blockFp.wait();
//     blockFp.wait();
//     blockFp.off();
//     migrationThread1.join();
//     migrationThread0.join();

//     // Verify that both migrations succeeded.
//     assert.commandWorked(migrationThread0.returnData());
//     assert.commandWorked(migrationThread1.returnData());
//     assert(donorsColl.findOne({tenantId: migrationOpts0.tenantId, state: "committed"}));
//     assert(donorsColl.findOne({tenantId: migrationOpts1.tenantId, state: "committed"}));

//     // Verify that the recipient RSM was only created once and was removed after both migrations
//     // finished.
//     const connPoolStatsAfter = assert.commandWorked(rst0Primary.adminCommand({connPoolStats:
//     1})); assert.eq(connPoolStatsAfter.numReplicaSetMonitorsCreated,
//               connPoolStatsBefore.numReplicaSetMonitorsCreated + 1);
//     assert.eq(Object.keys(connPoolStatsAfter.replicaSets).length, 0);
// })();

rst0.stopSet();
rst1.stopSet();
rst2.stopSet();
})();
