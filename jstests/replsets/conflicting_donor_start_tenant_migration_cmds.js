/**
 * Test that tenant migration donors correctly join retried donorStartMigration commands and reject
 * conflicting donorStartMigration commands.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft]
 */
(function() {
'use strict';

load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

/**
 * Starts a tenant migration on the given donor primary according the given migration options.
 */
function startMigration(donorPrimaryHost, migrationOpts) {
    const donorPrimary = new Mongo(donorPrimaryHost);
    return donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(migrationOpts.migrationIdString),
        recipientConnectionString: migrationOpts.recipientConnString,
        databasePrefix: migrationOpts.dbPrefix,
        readPreference: migrationOpts.readPreference
    });
}

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

const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
let charIndex = 0;

/**
 * Returns a database prefix that will not match any existing prefix.
 */
function generateUniqueDbPrefix() {
    assert.lt(charIndex, chars.length);
    return chars[charIndex++];
}

const rst0 = new ReplSetTest({nodes: 1, name: 'rst0'});
const rst1 = new ReplSetTest({nodes: 1, name: 'rst1'});
const rst2 = new ReplSetTest({nodes: 1, name: 'rst2'});

rst0.startSet();
rst0.initiate();

rst1.startSet();
rst1.initiate();

rst2.startSet();
rst2.initiate();

const rst0Primary = rst0.getPrimary();
const rst1Primary = rst1.getPrimary();

const kConfigDonorsNS = "config.tenantMigrationDonors";

let numRecipientSyncDataCmdSent = 0;

// Test that a retry of a donorStartMigration command joins the existing migration that has
// completed but has not been garbage-collected.
(() => {
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst1.getURL(),
        dbPrefix: generateUniqueDbPrefix() + "RetryAfterMigrationCompletes",
        readPreference: {mode: "primary"}
    };

    assert.commandWorked(startMigration(rst0Primary.host, migrationOpts));
    assert.commandWorked(startMigration(rst0Primary.host, migrationOpts));

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    numRecipientSyncDataCmdSent += 2;
    checkNumRecipientSyncDataCmdExecuted(rst1Primary, numRecipientSyncDataCmdSent);
})();

// Test that a retry of a donorStartMigration command joins the ongoing migration.
(() => {
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst1.getURL(),
        dbPrefix: generateUniqueDbPrefix() + "RetryBeforeMigrationCompletes",
        readPreference: {mode: "primary"}
    };

    let migrationThread0 = new Thread(startMigration, rst0Primary.host, migrationOpts);
    let migrationThread1 = new Thread(startMigration, rst0Primary.host, migrationOpts);

    migrationThread0.start();
    migrationThread1.start();
    migrationThread0.join();
    migrationThread1.join();

    assert.commandWorked(migrationThread0.returnData());
    assert.commandWorked(migrationThread1.returnData());

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    numRecipientSyncDataCmdSent += 2;
    checkNumRecipientSyncDataCmdExecuted(rst1Primary, numRecipientSyncDataCmdSent);
})();

/**
 * Tests that the donor throws a ConflictingOperationInProgress error if the client runs a
 * donorStartMigration command to start a migration that conflicts with an existing migration that
 * has committed but not garbage-collected (i.e. the donor has not received donorForgetMigration).
 */
function testStartingConflictingMigrationAfterInitialMigrationCommitted(
    donorPrimary, migrationOpts0, migrationOpts1) {
    assert.commandWorked(startMigration(donorPrimary.host, migrationOpts0));
    assert.commandFailedWithCode(startMigration(donorPrimary.host, migrationOpts1),
                                 ErrorCodes.ConflictingOperationInProgress);

    // If the second donorStartMigration had started a duplicate migration, there would be two donor
    // state docs.
    let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    assert.eq(1, configDonorsColl.count({databasePrefix: migrationOpts0.dbPrefix}));
}

/**
 * Tests that if the client runs multiple donorStartMigration commands that would start conflicting
 * migrations, only one of the migrations will start and succeed.
 */
function testConcurrentConflictingMigrations(donorPrimary, migrationOpts0, migrationOpts1) {
    let migrationThread0 = new Thread(startMigration, rst0Primary.host, migrationOpts0);
    let migrationThread1 = new Thread(startMigration, rst0Primary.host, migrationOpts1);

    migrationThread0.start();
    migrationThread1.start();
    migrationThread0.join();
    migrationThread1.join();

    const res0 = migrationThread0.returnData();
    const res1 = migrationThread1.returnData();
    let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);

    // Verify that only one migration succeeded.
    assert(res0.ok || res1.ok);
    assert(!res0.ok || !res1.ok);

    if (res0.ok) {
        assert.commandFailedWithCode(res1, ErrorCodes.ConflictingOperationInProgress);
        assert.eq(1, configDonorsColl.count({databasePrefix: migrationOpts0.dbPrefix}));
        if (migrationOpts0.dbPrefix != migrationOpts1.dbPrefix) {
            assert.eq(0, configDonorsColl.count({databasePrefix: migrationOpts1.dbPrefix}));
        }
    } else {
        assert.commandFailedWithCode(res0, ErrorCodes.ConflictingOperationInProgress);
        assert.eq(1, configDonorsColl.count({databasePrefix: migrationOpts1.dbPrefix}));
        if (migrationOpts0.dbPrefix != migrationOpts1.dbPrefix) {
            assert.eq(0, configDonorsColl.count({databasePrefix: migrationOpts0.dbPrefix}));
        }
    }
}

// Test migrations with different migrationIds but identical settings.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            dbPrefix: generateUniqueDbPrefix() + "DiffMigrationId",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.migrationIdString = extractUUIDFromObject(UUID());
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

// Test reusing a migrationId for different migration settings.

// Test different database prefixes.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            dbPrefix: generateUniqueDbPrefix() + "DiffDbPrefix",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.dbPrefix = generateUniqueDbPrefix() + "DiffDbPrefix";
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

// Test different recipient connection strings.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            dbPrefix: generateUniqueDbPrefix() + "DiffRecipientConnString",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.recipientConnString = rst2.getURL();
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

// Test different cloning read preference.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            dbPrefix: generateUniqueDbPrefix() + "DiffReadPref",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.readPreference = {mode: "secondary"};
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

rst0.stopSet();
rst1.stopSet();
rst2.stopSet();
})();
