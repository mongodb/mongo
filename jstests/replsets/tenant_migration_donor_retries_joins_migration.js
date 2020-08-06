/**
 * Tests that when the client retries donorStartMigration, the new command will rejoin the existing
 * migration if there is already a TenantMigrationAccessBlocker for that databasePrefix.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
"use strict";

const donorRst = new ReplSetTest(
    {nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}], name: 'donor'});
const recipientRst = new ReplSetTest({nodes: 1, name: 'recipient'});

const kDBPrefix = 'testDb';

let donorPrimary;
let recipientPrimary;
let kRecipientConnString;

const setup = () => {
    donorRst.startSet();
    donorRst.initiate();
    recipientRst.startSet();
    recipientRst.initiate();

    donorPrimary = donorRst.getPrimary();
    recipientPrimary = recipientRst.getPrimary();
    kRecipientConnString = recipientRst.getURL();
};
const tearDown = () => {
    donorRst.stopSet();
    recipientRst.stopSet();
};

const runDonorStartMigration = (primaryHost, recipientConnectionString, dbPrefix) => {
    return primaryHost.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: recipientConnectionString,
        databasePrefix: dbPrefix,
        readPreference: {mode: "primary"}
    });
};

(() => {
    // Test the case where the second donorStartMigration command joins the already active
    // migration.
    setup();
    const dbName = kDBPrefix + "Commit";

    jsTest.log('Runs initial tenant migration.');
    assert.commandWorked(runDonorStartMigration(donorPrimary, kRecipientConnString, dbName));

    jsTest.log('Re-joining active tenant migration.');
    assert.commandWorked(runDonorStartMigration(donorPrimary, kRecipientConnString, dbName));

    const donorStartMigrationMetrics =
        donorPrimary.adminCommand({serverStatus: 1}).metrics.commands.donorStartMigration;
    assert.eq(donorStartMigrationMetrics.failed, 0);
    assert.eq(donorStartMigrationMetrics.total, 2);

    // If the second donorStartMigration had run a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, 2);

    tearDown();
})();
})();
