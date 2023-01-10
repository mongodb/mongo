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

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {runMigrationAsync} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/rslib.js");  // 'createRstArgs'

const kTenantId = ObjectId().str;
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
const donorRstArgs = createRstArgs(donorRst);
const runMigrationThread =
    new Thread(runMigrationAsync, migrationOpts, donorRstArgs, {retryOnRetryableErrors: true});
runMigrationThread.start();

TenantMigrationTest.assertAborted(runMigrationThread.returnData());
tenantMigrationTest.waitForDonorNodesToReachState(
    donorRst.nodes, migrationId, migrationOpts.tenantId, TenantMigrationTest.DonorState.kAborted);
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
