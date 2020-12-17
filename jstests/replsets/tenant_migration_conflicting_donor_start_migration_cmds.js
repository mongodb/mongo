/**
 * Test that tenant migration donors correctly join retried donorStartMigration commands and reject
 * conflicting donorStartMigration commands.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */
(function() {
'use strict';

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

/**
 * Asserts that the number of recipientDataSync commands executed on the given recipient primary is
 * equal to the given number.
 */
function checkNumRecipientSyncDataCmdExecuted(recipientPrimary, expectedNumExecuted) {
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
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

const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
let charIndex = 0;

/**
 * Returns a tenantId that will not match any existing prefix.
 */
function generateUniqueTenantId() {
    assert.lt(charIndex, chars.length);
    return chars[charIndex++];
}

const donorRst = new ReplSetTest({nodes: 1, name: 'donorRst'});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest0 = new TenantMigrationTest({name: jsTestName(), donorRst});
if (!tenantMigrationTest0.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    return;
}

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = tenantMigrationTest0.getRecipientPrimary();

let numRecipientSyncDataCmdSent = 0;

// Test that a retry of a donorStartMigration command joins the existing migration that has
// completed but has not been garbage-collected.
(() => {
    const tenantId = `${generateUniqueTenantId()}RetryAfterMigrationCompletes`;
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    assert.commandWorked(tenantMigrationTest0.runMigration(migrationOpts));
    assert.commandWorked(tenantMigrationTest0.runMigration(migrationOpts));

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    numRecipientSyncDataCmdSent += 2;
    checkNumRecipientSyncDataCmdExecuted(recipientPrimary, numRecipientSyncDataCmdSent);
})();

// Test that a retry of a donorStartMigration command joins the ongoing migration.
(() => {
    const tenantId = `${generateUniqueTenantId()}RetryBeforeMigrationCompletes`;
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    assert.commandWorked(tenantMigrationTest0.startMigration(migrationOpts));
    assert.commandWorked(tenantMigrationTest0.startMigration(migrationOpts));

    assert.commandWorked(tenantMigrationTest0.waitForMigrationToComplete(migrationOpts));
    assert.commandWorked(tenantMigrationTest0.waitForMigrationToComplete(migrationOpts));

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    numRecipientSyncDataCmdSent += 2;
    checkNumRecipientSyncDataCmdExecuted(recipientPrimary, numRecipientSyncDataCmdSent);

    assert.commandWorked(tenantMigrationTest0.forgetMigration(migrationOpts.migrationIdString));
})();

/**
 * Tests that the donor throws a ConflictingOperationInProgress error if the client runs a
 * donorStartMigration command to start a migration that conflicts with an existing migration that
 * has committed but not garbage-collected (i.e. the donor has not received donorForgetMigration).
 */
function testStartingConflictingMigrationAfterInitialMigrationCommitted(
    tenantMigrationTest0, migrationOpts0, tenantMigrationTest1, migrationOpts1) {
    tenantMigrationTest0.runMigration(
        migrationOpts0, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */);
    assert.commandFailedWithCode(tenantMigrationTest1.runMigration(migrationOpts1),
                                 ErrorCodes.ConflictingOperationInProgress);

    // If the second donorStartMigration had started a duplicate migration, there would be two donor
    // state docs and TenantMigrationDonorService instances.
    let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    assert.eq(1, configDonorsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
    assert.eq(1, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                     "instanceID.uuid": UUID(migrationOpts0.migrationIdString)
                 }).length);
    if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
        assert.eq(0, configDonorsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
        assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         "instanceID.uuid": UUID(migrationOpts1.migrationIdString)
                     }).length);
    } else if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
        assert.eq(0, configDonorsColl.count({tenantId: migrationOpts1.tenantId}));
        assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         tenantId: migrationOpts1.tenantId
                     }).length);
    }
    assert.commandWorked(tenantMigrationTest0.forgetMigration(migrationOpts0.migrationIdString));
}

/**
 * Tests that if the client runs multiple donorStartMigration commands that would start conflicting
 * migrations, only one of the migrations will start and succeed.
 */
function testConcurrentConflictingMigrations(
    tenantMigrationTest0, migrationOpts0, tenantMigrationTest1, migrationOpts1) {
    const res0 = tenantMigrationTest0.startMigration(migrationOpts0);
    const res1 = tenantMigrationTest1.startMigration(migrationOpts1);

    let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);

    // Verify that exactly one migration succeeded.
    assert(res0.ok || res1.ok);
    assert(!res0.ok || !res1.ok);

    if (res0.ok) {
        assert.commandFailedWithCode(res1, ErrorCodes.ConflictingOperationInProgress);
        assert.eq(1, configDonorsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
        assert.eq(1, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         "instanceID.uuid": UUID(migrationOpts0.migrationIdString)
                     }).length);
        if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
            assert.eq(0, configDonorsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             "instanceID.uuid": UUID(migrationOpts1.migrationIdString)
                         }).length);
        } else if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
            assert.eq(0, configDonorsColl.count({tenantId: migrationOpts1.tenantId}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             tenantId: migrationOpts1.tenantId
                         }).length);
        }
        assert.commandWorked(tenantMigrationTest0.waitForMigrationToComplete(migrationOpts0));
        assert.commandWorked(
            tenantMigrationTest0.forgetMigration(migrationOpts0.migrationIdString));
    } else {
        assert.commandFailedWithCode(res0, ErrorCodes.ConflictingOperationInProgress);
        assert.eq(1, configDonorsColl.count({_id: UUID(migrationOpts1.migrationIdString)}));
        assert.eq(1, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                         "instanceID.uuid": UUID(migrationOpts1.migrationIdString)
                     }).length);
        if (migrationOpts0.migrationIdString != migrationOpts1.migrationIdString) {
            assert.eq(0, configDonorsColl.count({_id: UUID(migrationOpts0.migrationIdString)}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             "instanceID.uuid": UUID(migrationOpts0.migrationIdString)
                         }).length);
        } else if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
            assert.eq(0, configDonorsColl.count({tenantId: migrationOpts0.tenantId}));
            assert.eq(0, getTenantMigrationDonorCurrentOpEntries(donorPrimary, {
                             tenantId: migrationOpts0.tenantId
                         }).length);
        }
        assert.commandWorked(tenantMigrationTest1.waitForMigrationToComplete(migrationOpts1));
        assert.commandWorked(
            tenantMigrationTest1.forgetMigration(migrationOpts1.migrationIdString));
    }
}

// Test migrations with different migrationIds but identical settings.
(() => {
    let makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffMigrationId",
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.migrationIdString = extractUUIDFromObject(UUID());
        return [tenantMigrationTest0, migrationOpts0, tenantMigrationTest0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(...makeTestParams());
    testConcurrentConflictingMigrations(...makeTestParams());
})();

// Test reusing a migrationId for different migration settings.

// Test different tenantIds.
(() => {
    let makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffTenantId",
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.tenantId = generateUniqueTenantId() + "DiffTenantId";
        return [tenantMigrationTest0, migrationOpts0, tenantMigrationTest0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(...makeTestParams());
    testConcurrentConflictingMigrations(...makeTestParams());
})();

// Test different recipient connection strings.
(() => {
    const tenantMigrationTest1 = new TenantMigrationTest({name: `${jsTestName()}1`, donorRst});

    let makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffRecipientConnString",
        };
        // The recipient connection string will be populated by the TenantMigrationTest fixture, so
        // no need to set it here.
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        return [tenantMigrationTest0, migrationOpts0, tenantMigrationTest1, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(...makeTestParams());
    testConcurrentConflictingMigrations(...makeTestParams());

    tenantMigrationTest1.stop();
})();

// Test different cloning read preference.
(() => {
    let makeTestParams = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            tenantId: generateUniqueTenantId() + "DiffReadPref",
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.readPreference = {mode: "secondary"};
        return [tenantMigrationTest0, migrationOpts0, tenantMigrationTest0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(...makeTestParams());
    testConcurrentConflictingMigrations(...makeTestParams());
})();

tenantMigrationTest0.stop();
donorRst.stopSet();
})();
