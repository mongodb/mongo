/**
 * Test that tenant migration recipient rejects conflicting recipientSyncData commands.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */
(function() {

"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/replsets/libs/tenant_migration_util.js");

var rst =
    new ReplSetTest({nodes: 1, nodeOptions: TenantMigrationUtil.makeX509OptionsForTest().donor});
rst.startSet();
rst.initiate();
if (!TenantMigrationUtil.isFeatureFlagEnabled(rst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    rst.stopSet();
    return;
}

const primary = rst.getPrimary();
const configDB = primary.getDB("config");
const tenantMigrationRecipientStateColl = configDB["tenantMigrationRecipients"];

const tenantId = "test";
const connectionString = "foo/bar:12345";
const readPreference = {
    mode: 'primary'
};

TestData.stopFailPointErrorCode = 4880402;
function checkTenantMigrationRecipientStateCollCount(expectedCount) {
    let res = tenantMigrationRecipientStateColl.find().toArray();
    assert.eq(expectedCount,
              res.length,
              "'config.tenantMigrationRecipients' collection count mismatch: " + tojson(res));
}

/**
 * Returns an array of currentOp entries for the TenantMigrationRecipientService instances that
 * match the given query.
 */
function getTenantMigrationRecipientCurrentOpEntries(recipientPrimary, query) {
    const cmdObj = Object.assign({currentOp: true, desc: "tenant recipient migration"}, query);
    return assert.commandWorked(recipientPrimary.adminCommand(cmdObj)).inprog;
}

function startRecipientSyncDataCmd(migrationUuid, tenantId, connectionString, readPreference) {
    load("jstests/replsets/libs/tenant_migration_util.js");

    jsTestLog("Starting a recipientSyncDataCmd for migrationUuid: " + migrationUuid +
              " tenantId: '" + tenantId + "'");
    assert.commandFailedWithCode(
        db.adminCommand({
            recipientSyncData: 1,
            migrationId: migrationUuid,
            donorConnectionString: connectionString,
            tenantId: tenantId,
            readPreference: readPreference,
            recipientCertificateForDonor:
                TenantMigrationUtil.makeMigrationCertificatesForTest().recipientCertificateForDonor
        }),
        [TestData.stopFailPointErrorCode, ErrorCodes.ConflictingOperationInProgress]);
}

// Enable the failpoint to stop the tenant migration after persisting the state doc.
assert.commandWorked(primary.adminCommand({
    configureFailPoint: "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
    mode: "alwaysOn",
    data: {action: "stop", stopErrorCode: NumberInt(TestData.stopFailPointErrorCode)}
}));

{
    // Enable failPoint to pause the migration just as it starts.
    const fpPauseBeforeRunTenantMigrationRecipientInstance =
        configureFailPoint(primary, "pauseBeforeRunTenantMigrationRecipientInstance");

    // Sanity check : 'config.tenantMigrationRecipients' collection count should be empty.
    checkTenantMigrationRecipientStateCollCount(0);
    // Start the  conflicting recipientSyncData cmds.
    const recipientSyncDataCmd1 = startParallelShell(
        funWithArgs(startRecipientSyncDataCmd, UUID(), tenantId, connectionString, readPreference),
        primary.port);
    const recipientSyncDataCmd2 = startParallelShell(
        funWithArgs(startRecipientSyncDataCmd, UUID(), tenantId, connectionString, readPreference),
        primary.port);

    jsTestLog("Waiting until both conflicting instances get started and hit the failPoint.");
    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "pauseBeforeRunTenantMigrationRecipientInstance",
        timesEntered: fpPauseBeforeRunTenantMigrationRecipientInstance.timesEntered + 2,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // Two instances are expected as the tenantId conflict is still unresolved.
    jsTestLog("Fetching current operations before conflict is resolved.");
    const currentOpEntriesBeforeInsert = getTenantMigrationRecipientCurrentOpEntries(
        primary, {desc: "tenant recipient migration", tenantId});
    assert.eq(2, currentOpEntriesBeforeInsert.length, tojson(currentOpEntriesBeforeInsert));

    jsTestLog("Unblocking the tenant migration instance from persisting the state doc.");
    fpPauseBeforeRunTenantMigrationRecipientInstance.off();

    // Wait for both the conflicting instances to complete. Although both will "complete", one will
    // return with ErrorCodes.ConflictingOperationInProgress, and the other with a
    // TestData.stopFailPointErrorCode (a failpoint indicating that we have persisted the document).
    recipientSyncDataCmd1();
    recipientSyncDataCmd2();

    // One of the two instances should have been cleaned up, and therefore only one will remain.
    const currentOpEntriesAfterInsert = getTenantMigrationRecipientCurrentOpEntries(
        primary, {desc: "tenant recipient migration", tenantId});
    assert.eq(1, currentOpEntriesAfterInsert.length, tojson(currentOpEntriesAfterInsert));

    // Only one instance should have succeeded in persisting the state doc, other should have failed
    // with ErrorCodes.ConflictingOperationInProgress.
    checkTenantMigrationRecipientStateCollCount(1);
}

{
    // Now, again call recipientSyncData cmd  to run on the same tenant "test'. Since, our previous
    // instance for  tenant "test' wasn't garbage collected, the migration status for that tenant is
    // considered as active. So, this command should fail with
    // ErrorCodes.ConflictingOperationInProgress.
    const recipientSyncDataCmd3 = startParallelShell(
        funWithArgs(startRecipientSyncDataCmd, UUID(), tenantId, connectionString, readPreference),
        primary.port);
    recipientSyncDataCmd3();

    // Collection count should remain the same.
    checkTenantMigrationRecipientStateCollCount(1);
}

rst.stopSet();
})();
