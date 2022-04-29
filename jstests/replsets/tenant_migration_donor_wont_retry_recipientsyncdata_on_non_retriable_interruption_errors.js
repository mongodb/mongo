/**
 * Tests that a tenant migration will be aborted when the recipient returns a non-retriable
 * 'interruption' error for the 'recipientSyncData' command. This is to avoid situations like
 * SERVER-58398.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   incompatible_with_windows_tls,
 *   incompatible_with_macos,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";
const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const donorRst = tenantMigrationTest.getDonorRst();
let recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const interruptionErrorCode = ErrorCodes.MaxTimeMSExpired;
assert(ErrorCodes.isInterruption(interruptionErrorCode));
configureFailPoint(recipientPrimary, "failCommand", {
    failInternalCommands: true,
    errorCode: interruptionErrorCode,
    failCommands: ["recipientSyncData"],
});

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
};
const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
const runMigrationThread = new Thread(TenantMigrationUtil.runMigrationAsync,
                                      migrationOpts,
                                      donorRstArgs,
                                      {retryOnRetryableErrors: true});
runMigrationThread.start();

TenantMigrationTest.assertAborted(runMigrationThread.returnData());
tenantMigrationTest.waitForDonorNodesToReachState(
    donorRst.nodes, migrationId, migrationOpts.tenantId, TenantMigrationTest.DonorState.kAborted);
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
