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
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {runMigrationAsync} from "jstests/replsets/libs/tenant_migration_util.js";
import {createRstArgs} from "jstests/replsets/rslib.js";

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
