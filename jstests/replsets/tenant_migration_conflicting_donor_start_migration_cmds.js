/**
 * Test that tenant migration donors correctly join retried donorStartMigration commands and reject
 * conflicting donorStartMigration commands.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

function getRecipientSyncDataMetrics(recipientPrimary) {
    return recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
}

/**
 * Asserts that the number of recipientDataSync commands executed on the given recipient primary is
 * equal to the given number.
 */
function checkNumRecipientSyncDataCmdExecuted(recipientPrimary, expectedNumExecuted) {
    const recipientSyncDataMetrics = getRecipientSyncDataMetrics(recipientPrimary);
    assert.eq(0, recipientSyncDataMetrics.failed);
    assert.eq(expectedNumExecuted, recipientSyncDataMetrics.total);
}

/**
 * Returns an array of currentOp entries for the TenantMigrationDonorService instances that match
 * the given query.
 */
function getTenantMigrationDonorCurrentOpEntries(donorPrimary, query) {
    const cmdObj = Object.assign({currentOp: true, desc: "tenant donor migration"}, query);
    return assert.commandWorked(donorPrimary.adminCommand(cmdObj)).inprog;
}

/**
 * Asserts that the string does not contain certificate or private pem string.
 */
function assertNoCertificateOrPrivateKey(string) {
    assert(!string.includes("CERTIFICATE"), "found certificate");
    assert(!string.includes("PRIVATE KEY"), "found private key");
}

const kTenantIdPrefix = "testTenantId";
let tenantCounter = 0;

/**
 * Returns a tenantId that will not match any existing prefix.
 */
function generateUniqueTenantId() {
    return kTenantIdPrefix + tenantCounter++;
}

const {donor: donorNodeOptions} = TenantMigrationUtil.makeX509OptionsForTest();
donorNodeOptions.setParameter = donorNodeOptions.setParameter || {};
Object.assign(donorNodeOptions.setParameter, {
    tenantMigrationGarbageCollectionDelayMS: 1 * 1000,
    ttlMonitorSleepSecs: 1,
});
const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donorRst',
    nodeOptions: donorNodeOptions,
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), donorRst, quickGarbageCollection: true});

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

function teardown() {
    tenantMigrationTest.stop();
    donorRst.stopSet();
}

// Test that a retry of a donorStartMigration command joins the existing migration that has
// completed but has not been garbage-collected.
(() => {
    const tenantId = `${generateUniqueTenantId()}RetryAfterMigrationCompletes`;
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };

    const recipientSyncDataMetrics = getRecipientSyncDataMetrics(recipientPrimary);

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    checkNumRecipientSyncDataCmdExecuted(recipientPrimary, recipientSyncDataMetrics.total + 2);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);
})();

// Test that a retry of a donorStartMigration command joins the ongoing migration.
(() => {
    const tenantId = `${generateUniqueTenantId()}RetryBeforeMigrationCompletes`;
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };

    const recipientSyncDataMetrics = getRecipientSyncDataMetrics(recipientPrimary);

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    checkNumRecipientSyncDataCmdExecuted(recipientPrimary, recipientSyncDataMetrics.total + 2);

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);
})();

/**
 * Tests that the donor throws a ConflictingOperationInProgress error if the client runs a
 * donorStartMigration command to start a migration that conflicts with an existing migration that
 * has committed but not garbage-collected (i.e. the donor has not received donorForgetMigration).
 */
function testStartingConflictingMigrationAfterInitialMigrationCommitted({
    tenantMigrationTest0,
    migrationOpts0,
    tenantMigrationTest1,
    migrationOpts1,
    donorPrimary,
}) {
    jsTestLog("Start conflicting migration after first commits");
    const res0 =
        tenantMigrationTest0.runMigration(migrationOpts0, {automaticForgetMigration: false});
    jsTestLog(`Migration 0 opts: ${tojson(migrationOpts0)}, result: ${tojson(res0)}`);
    TenantMigrationTest.assertCommitted(res0);
    const res1 = tenantMigrationTest1.runMigration(migrationOpts1);
    jsTestLog(`Migration 1 opts: ${tojson(migrationOpts1)}, result: ${tojson(res1)}`);
    assert.commandFailedWithCode(res1, ErrorCodes.ConflictingOperationInProgress);
    assertNoCertificateOrPrivateKey(res1.errmsg);

    // If the second donorStartMigration had started a duplicate migration, there would be two donor
    // state docs and TenantMigrationDonorService instances.
    let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert.eq(1, configDonorsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
    assert.eq(1, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                     "instanceID": UUID(migrationOpts0.migrationIdString)
                 }).length);
    if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
        assert.eq(0, configDonorsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
        assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         "instanceID": UUID(migrationOpts1.migrationIdString)
                     }).length);
    } else if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
        assert.eq(0, configDonorsColl.count({tenantId: migrationOpts1.tenantId}));
        assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         tenantId: migrationOpts1.tenantId
                     }).length);
    }
    assert.commandWorked(tenantMigrationTest0.forgetMigration(migrationOpts0.migrationIdString));
    tenantMigrationTest0.waitForMigrationGarbageCollection(migrationOpts0.migrationIdString,
                                                           migrationOpts0.tenantId);
}

/**
 * Tests that if the client runs multiple donorStartMigration commands that would start conflicting
 * migrations, only one of the migrations will start and succeed.
 */
function testConcurrentConflictingMigrations({
    tenantMigrationTest0,
    migrationOpts0,
    tenantMigrationTest1,
    migrationOpts1,
    donorPrimary,
}) {
    jsTestLog("Start conflicting migrations concurrently");
    const res0 = tenantMigrationTest0.startMigration(migrationOpts0);
    jsTestLog(`Migration 0 opts: ${tojson(migrationOpts0)}, result: ${tojson(res0)}`);
    const res1 = tenantMigrationTest1.startMigration(migrationOpts1);
    jsTestLog(`Migration 1 opts: ${tojson(migrationOpts1)}, result: ${tojson(res1)}`);

    let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);

    // Verify that exactly one migration succeeded.
    assert(res0.ok || res1.ok);
    assert(!res0.ok || !res1.ok);

    if (res0.ok) {
        assert.commandFailedWithCode(res1, ErrorCodes.ConflictingOperationInProgress);
        assertNoCertificateOrPrivateKey(res1.errmsg);
        assert.eq(1, configDonorsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
        assert.eq(1, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         "instanceID": UUID(migrationOpts0.migrationIdString)
                     }).length);
        if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
            assert.eq(0, configDonorsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             "instanceID": UUID(migrationOpts1.migrationIdString)
                         }).length);
        } else if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
            assert.eq(0, configDonorsColl.count({tenantId: migrationOpts1.tenantId}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             tenantId: migrationOpts1.tenantId
                         }).length);
        }
        TenantMigrationTest.assertCommitted(
            tenantMigrationTest0.waitForMigrationToComplete(migrationOpts0));
        assert.commandWorked(
            tenantMigrationTest0.forgetMigration(migrationOpts0.migrationIdString));
        tenantMigrationTest0.waitForMigrationGarbageCollection(migrationOpts0.migrationIdString,
                                                               migrationOpts0.tenantId);
    } else {
        assert.commandFailedWithCode(res0, ErrorCodes.ConflictingOperationInProgress);
        assertNoCertificateOrPrivateKey(res0.errmsg);
        assert.eq(1, configDonorsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
        assert.eq(1, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         "instanceID": UUID(migrationOpts1.migrationIdString)
                     }).length);
        if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
            assert.eq(0, configDonorsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             "instanceID": UUID(migrationOpts0.migrationIdString)
                         }).length);
        } else if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
            assert.eq(0, configDonorsColl.count({tenantId: migrationOpts0.tenantId}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             tenantId: migrationOpts0.tenantId
                         }).length);
        }
        TenantMigrationTest.assertAborted(
            tenantMigrationTest1.waitForMigrationToComplete(migrationOpts1));
        assert.commandWorked(
            tenantMigrationTest1.forgetMigration(migrationOpts1.migrationIdString));
        tenantMigrationTest1.waitForMigrationGarbageCollection(migrationOpts1.migrationIdString,
                                                               migrationOpts1.tenantId);
    }
}

// Test migrations with different migrationIds but identical settings.
(() => {
    const makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffMigrationId",
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.migrationIdString = extractUUIDFromObject(UUID());
        return {
            tenantMigrationTest0: tenantMigrationTest,
            migrationOpts0,
            tenantMigrationTest1: tenantMigrationTest,
            migrationOpts1,
            donorPrimary,
        };
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(makeTestParams());
    testConcurrentConflictingMigrations(makeTestParams());
})();

// Test reusing a migrationId for different migration settings.

// Test different tenantIds.
(() => {
    if (TenantMigrationUtil.isShardMergeEnabled(donorPrimary.getDB("admin"))) {
        jsTestLog(
            "Skip: featureFlagShardMerge is enabled and this test tests migrations with different tenant ids.");
        return;
    }

    const makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffTenantId",
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.tenantId = generateUniqueTenantId() + "DiffTenantId";
        return {
            tenantMigrationTest0: tenantMigrationTest,
            migrationOpts0,
            tenantMigrationTest1: tenantMigrationTest,
            migrationOpts1,
            donorPrimary,
        };
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(makeTestParams());
    testConcurrentConflictingMigrations(makeTestParams());
})();

// Test different recipient connection strings.
(() => {
    const tenantMigrationTest1 = new TenantMigrationTest({name: `${jsTestName()}1`, donorRst});

    const makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: `${generateUniqueTenantId()}DiffRecipientConnString`,
        };
        // The recipient connection string will be populated by the TenantMigrationTest fixture, so
        // no need to set it here.
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        return {
            tenantMigrationTest0: tenantMigrationTest,
            migrationOpts0,
            tenantMigrationTest1,
            migrationOpts1,
            donorPrimary,
        };
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(makeTestParams());
    testConcurrentConflictingMigrations(makeTestParams());

    tenantMigrationTest1.stop();
})();

// Test different cloning read preference.
(() => {
    if (TenantMigrationUtil.isShardMergeEnabled(donorPrimary.getDB("admin"))) {
        jsTestLog(
            "Skip: featureFlagShardMerge is enabled and this test tests migration's secondary read preference.");
        return;
    }
    const makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffReadPref",
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.readPreference = {mode: "secondary"};
        return {
            tenantMigrationTest0: tenantMigrationTest,
            migrationOpts0,
            tenantMigrationTest1: tenantMigrationTest,
            migrationOpts1,
            donorPrimary,
        };
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(makeTestParams());
    testConcurrentConflictingMigrations(makeTestParams());
})();

const kDonorCertificateAndPrivateKey =
    TenantMigrationUtil.getCertificateAndPrivateKey("jstests/libs/tenant_migration_donor.pem");
const kExpiredDonorCertificateAndPrivateKey = TenantMigrationUtil.getCertificateAndPrivateKey(
    "jstests/libs/tenant_migration_donor_expired.pem");
const kRecipientCertificateAndPrivateKey =
    TenantMigrationUtil.getCertificateAndPrivateKey("jstests/libs/tenant_migration_recipient.pem");
const kExpiredRecipientCertificateAndPrivateKey = TenantMigrationUtil.getCertificateAndPrivateKey(
    "jstests/libs/tenant_migration_recipient_expired.pem");

// Test different donor certificates.
(() => {
    const makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffDonorCertificate",
            donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
            recipientCertificateForDonor: kRecipientCertificateAndPrivateKey
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.donorCertificateForRecipient = kExpiredDonorCertificateAndPrivateKey;
        return {
            tenantMigrationTest0: tenantMigrationTest,
            migrationOpts0,
            tenantMigrationTest1: tenantMigrationTest,
            migrationOpts1,
            donorPrimary,
        };
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(makeTestParams());
    testConcurrentConflictingMigrations(makeTestParams());
})();

// Test different recipient certificates.
(() => {
    const makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffRecipientCertificate",
            donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
            recipientCertificateForDonor: kRecipientCertificateAndPrivateKey
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.recipientCertificateForDonor = kExpiredRecipientCertificateAndPrivateKey;
        return {
            tenantMigrationTest0: tenantMigrationTest,
            migrationOpts0,
            tenantMigrationTest1: tenantMigrationTest,
            migrationOpts1,
            donorPrimary,
        };
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(makeTestParams());
    testConcurrentConflictingMigrations(makeTestParams());
})();

teardown();
})();
