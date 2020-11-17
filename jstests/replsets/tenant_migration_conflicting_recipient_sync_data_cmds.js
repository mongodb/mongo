/**
 * Test that tenant migration recipient rejects conflicting recipientSyncData commands.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */
(function() {

"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/curop_helpers.js");  // for waitForCurOpByFailPoint().
load("jstests/replsets/libs/tenant_migration_util.js");

var rst = new ReplSetTest({nodes: 1});
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
const tenantMigrationRecipientStateCollNss = tenantMigrationRecipientStateColl.getFullName();

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

function startRecipientSyncDataCmd(migrationUuid, tenantId, connectionString, readPreference) {
    jsTestLog("Starting a recipientSyncDataCmd for migrationUuid: " + migrationUuid +
              " tenantId: '" + tenantId + "'");
    assert.commandFailedWithCode(
        db.adminCommand({
            recipientSyncData: 1,
            migrationId: migrationUuid,
            donorConnectionString: connectionString,
            tenantId: tenantId,
            readPreference: readPreference
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
    // Enable the failpoint before inserting the state document by upsert command.
    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: "hangBeforeUpsertPerformsInsert", mode: "alwaysOn"}));

    // Sanity check : 'config.tenantMigrationRecipients' collection count should be empty.
    checkTenantMigrationRecipientStateCollCount(0);

    // Start the  conflicting recipientSyncData cmds.
    const recipientSyncDataCmd1 = startParallelShell(
        funWithArgs(startRecipientSyncDataCmd, UUID(), tenantId, connectionString, readPreference),
        primary.port);
    const recipientSyncDataCmd2 = startParallelShell(
        funWithArgs(startRecipientSyncDataCmd, UUID(), tenantId, connectionString, readPreference),
        primary.port);

    // Wait until both the conflicting instances got started.
    checkLog.containsWithCount(primary, "Starting tenant migration recipient instance", 2);

    jsTestLog("Waiting for 'hangBeforeUpsertPerformsInsert' failpoint to reach");
    waitForCurOpByFailPoint(
        configDB, tenantMigrationRecipientStateCollNss, "hangBeforeUpsertPerformsInsert");

    // Unblock the tenant migration instance from persisting the state doc.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "hangBeforeUpsertPerformsInsert", mode: "off"}));

    // Wait for both the conflicting instances to complete.
    recipientSyncDataCmd1();
    recipientSyncDataCmd2();

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
