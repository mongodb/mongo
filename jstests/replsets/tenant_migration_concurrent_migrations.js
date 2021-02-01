/**
 * Test that multiple concurrent tenant migrations are supported.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const x509Options0 = TenantMigrationUtil.makeX509Options("jstests/libs/rs0.pem");
const x509Options1 = TenantMigrationUtil.makeX509Options("jstests/libs/rs1.pem");
const x509Options2 = TenantMigrationUtil.makeX509Options("jstests/libs/rs2.pem");

const migrationCertificate0 =
    TenantMigrationUtil.getCertificateAndPrivateKey("jstests/libs/rs0_tenant_migration.pem");
const migrationCertificate1 =
    TenantMigrationUtil.getCertificateAndPrivateKey("jstests/libs/rs1_tenant_migration.pem");
const migrationCertificate2 =
    TenantMigrationUtil.getCertificateAndPrivateKey("jstests/libs/rs2_tenant_migration.pem");

const rst0 = new ReplSetTest({nodes: 1, name: 'rst0', nodeOptions: x509Options0});
const rst1 = new ReplSetTest({nodes: 1, name: 'rst1', nodeOptions: x509Options1});
const rst2 = new ReplSetTest({nodes: 1, name: 'rst2', nodeOptions: x509Options2});

rst0.startSet();
rst0.initiate();

rst1.startSet();
rst1.initiate();

rst2.startSet();
rst2.initiate();

const kTenantIdPrefix = "testTenantId";

// Test concurrent outgoing migrations to different recipients.
(() => {
    const tenantMigrationTest0 = new TenantMigrationTest({donorRst: rst0, recipientRst: rst1});
    if (!tenantMigrationTest0.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const tenantMigrationTest1 = new TenantMigrationTest({donorRst: rst0, recipientRst: rst2});
    const tenantId0 = `${kTenantIdPrefix}-ConcurrentOutgoingMigrationsToDifferentRecipient0`;
    const tenantId1 = `${kTenantIdPrefix}-ConcurrentOutgoingMigrationsToDifferentRecipient1`;
    const donorPrimary = rst0.getPrimary();
    const connPoolStatsBefore = assert.commandWorked(donorPrimary.adminCommand({connPoolStats: 1}));

    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId0,
        donorCertificateForRecipient: migrationCertificate0,
        recipientCertificateForDonor: migrationCertificate1,
    };
    const migrationOpts1 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId1,
        donorCertificateForRecipient: migrationCertificate0,
        recipientCertificateForDonor: migrationCertificate2,
    };

    assert.commandWorked(tenantMigrationTest0.startMigration(migrationOpts0));
    assert.commandWorked(tenantMigrationTest1.startMigration(migrationOpts1));

    const stateRes0 =
        assert.commandWorked(tenantMigrationTest0.waitForMigrationToComplete(migrationOpts0));
    const stateRes1 =
        assert.commandWorked(tenantMigrationTest1.waitForMigrationToComplete(migrationOpts1));

    // Verify that both migrations succeeded.
    assert.eq(stateRes0.state, TenantMigrationTest.State.kCommitted);
    assert.eq(stateRes1.state, TenantMigrationTest.State.kCommitted);

    const connPoolStatsAfter0 = assert.commandWorked(donorPrimary.adminCommand({connPoolStats: 1}));
    // Donor targeted two different replica sets.
    assert.eq(connPoolStatsAfter0.numReplicaSetMonitorsCreated,
              connPoolStatsBefore.numReplicaSetMonitorsCreated + 2);
    assert.eq(Object.keys(connPoolStatsAfter0.replicaSets).length, 2);

    assert.commandWorked(tenantMigrationTest0.forgetMigration(migrationOpts0.migrationIdString));
    assert.commandWorked(tenantMigrationTest1.forgetMigration(migrationOpts1.migrationIdString));

    // After migrations are complete, RSMs are garbage collected.
    const connPoolStatsAfter1 = assert.commandWorked(donorPrimary.adminCommand({connPoolStats: 1}));
    assert.eq(Object.keys(connPoolStatsAfter1.replicaSets).length, 0);

    assert.eq(Object
                  .keys(assert.commandWorked(rst1.getPrimary().adminCommand({connPoolStats: 1}))
                            .replicaSets)
                  .length,
              0);
})();

// Test concurrent incoming migrations from different donors.
(() => {
    const tenantMigrationTest0 = new TenantMigrationTest({donorRst: rst0, recipientRst: rst2});
    if (!tenantMigrationTest0.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const tenantMigrationTest1 = new TenantMigrationTest({donorRst: rst1, recipientRst: rst2});
    const tenantId0 = `${kTenantIdPrefix}-ConcurrentIncomingMigrations0`;
    const tenantId1 = `${kTenantIdPrefix}-ConcurrentIncomingMigrations1`;

    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId0,
        donorCertificateForRecipient: migrationCertificate0,
        recipientCertificateForDonor: migrationCertificate2,
    };
    const migrationOpts1 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId1,
        donorCertificateForRecipient: migrationCertificate1,
        recipientCertificateForDonor: migrationCertificate2,
    };

    assert.commandWorked(tenantMigrationTest0.startMigration(migrationOpts0));
    assert.commandWorked(tenantMigrationTest1.startMigration(migrationOpts1));

    const stateRes0 =
        assert.commandWorked(tenantMigrationTest0.waitForMigrationToComplete(migrationOpts0));
    const stateRes1 =
        assert.commandWorked(tenantMigrationTest1.waitForMigrationToComplete(migrationOpts1));

    // Verify that both migrations succeeded.
    assert.eq(stateRes0.state, TenantMigrationTest.State.kCommitted);
    assert.eq(stateRes1.state, TenantMigrationTest.State.kCommitted);

    // Cleanup.
    assert.commandWorked(tenantMigrationTest0.forgetMigration(migrationOpts0.migrationIdString));
    assert.commandWorked(tenantMigrationTest1.forgetMigration(migrationOpts1.migrationIdString));

    const connPoolStatsAfter0 =
        assert.commandWorked(rst0.getPrimary().adminCommand({connPoolStats: 1}));
    assert.eq(Object.keys(connPoolStatsAfter0.replicaSets).length, 0);

    const connPoolStatsAfter1 =
        assert.commandWorked(rst1.getPrimary().adminCommand({connPoolStats: 1}));
    assert.eq(Object.keys(connPoolStatsAfter1.replicaSets).length, 0);
})();

// Test concurrent outgoing migrations to same recipient. Verify that tenant
// migration donor only removes a ReplicaSetMonitor for a recipient when the last
// migration to that recipient completes.
(() => {
    const tenantMigrationTest0 = new TenantMigrationTest({donorRst: rst0, recipientRst: rst1});
    if (!tenantMigrationTest0.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const tenantMigrationTest1 = new TenantMigrationTest({donorRst: rst0, recipientRst: rst1});

    const tenantId0 = `${kTenantIdPrefix}-ConcurrentOutgoingMigrationsToSameRecipient0`;
    const tenantId1 = `${kTenantIdPrefix}-ConcurrentOutgoingMigrationsToSameRecipient1`;

    const donorsColl = tenantMigrationTest0.getDonorRst().getPrimary().getCollection(
        TenantMigrationTest.kConfigDonorsNS);

    const migrationOpts0 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId0,
        donorCertificateForRecipient: migrationCertificate0,
        recipientCertificateForDonor: migrationCertificate1,
    };
    const migrationOpts1 = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId1,
        donorCertificateForRecipient: migrationCertificate0,
        recipientCertificateForDonor: migrationCertificate1,
    };

    const donorPrimary = rst0.getPrimary();

    const connPoolStatsBefore = assert.commandWorked(donorPrimary.adminCommand({connPoolStats: 1}));

    let blockFp = configureFailPoint(
        donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState", {tenantId: tenantId1});
    assert.commandWorked(tenantMigrationTest0.startMigration(migrationOpts0));
    assert.commandWorked(tenantMigrationTest1.startMigration(migrationOpts1));

    // Wait migration1 to pause in the blocking state and for migration0 to commit.
    blockFp.wait();
    const stateRes0 =
        assert.commandWorked(tenantMigrationTest0.waitForMigrationToComplete(migrationOpts0));
    assert(stateRes0.state, TenantMigrationTest.State.kCommitted);

    // Verify that exactly one RSM was created.
    const connPoolStatsDuringMigration =
        assert.commandWorked(donorPrimary.adminCommand({connPoolStats: 1}));
    assert.eq(connPoolStatsDuringMigration.numReplicaSetMonitorsCreated,
              connPoolStatsBefore.numReplicaSetMonitorsCreated + 1);
    assert.eq(Object.keys(connPoolStatsDuringMigration.replicaSets).length, 1);

    // Garbage collect migration0 and verify that the RSM was not removed.
    assert.commandWorked(tenantMigrationTest0.forgetMigration(migrationOpts0.migrationIdString));
    assert.eq(
        Object.keys(assert.commandWorked(donorPrimary.adminCommand({connPoolStats: 1})).replicaSets)
            .length,
        1);

    // Let the migration1 to finish.
    blockFp.off();
    const stateRes1 =
        assert.commandWorked(tenantMigrationTest1.waitForMigrationToComplete(migrationOpts1));
    assert(stateRes1.state, TenantMigrationTest.State.kCommitted);

    // Verify that now the RSM is garbage collected after the migration1 is cleaned.
    assert.commandWorked(tenantMigrationTest1.forgetMigration(migrationOpts1.migrationIdString));

    assert.eq(
        Object.keys(assert.commandWorked(donorPrimary.adminCommand({connPoolStats: 1})).replicaSets)
            .length,
        0);
})();

rst0.stopSet();
rst1.stopSet();
rst2.stopSet();
})();
