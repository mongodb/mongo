/**
 * Tests that a recipientForgetMigration is received after the recipient state doc has been deleted.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   # Shard merge protocol will be tested by
 *   # tenant_migration_shard_merge_recipient_retry_forget_migration.js.
 *   incompatible_with_shard_merge,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 *   # The currentOp output field 'state' was changed from an enum value to a string.
 *   requires_fcv_70,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled, makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const migrationId = UUID();
const tenantId = ObjectId().str;

const dbName = makeTenantDB(tenantId, "test");
const collName = "coll";

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

if (isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping this shard merge incompatible test.");
    quit();
}

// Not doing a migration before writing to the recipient to mimic that a migration has completed and
// the state doc has been garbage collected.
assert.commandWorked(recipientPrimary.getDB(dbName)[collName].insert({_id: 1}));

function runRecipientForgetMigration(host, {
    migrationIdString,
    donorConnectionString,
    tenantId,
    readPreference,
}) {
    const db = new Mongo(host);
    return db.adminCommand({
        recipientForgetMigration: 1,
        migrationId: UUID(migrationIdString),
        donorConnectionString,
        tenantId,
        readPreference,
    });
}

const fp = configureFailPoint(recipientPrimary, "hangBeforeTaskCompletion");

const recipientForgetMigrationThread =
    new Thread(runRecipientForgetMigration, recipientPrimary.host, {
        migrationIdString: extractUUIDFromObject(migrationId),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantId,
        readPreference: {mode: "primary"},
    });

// Run a delayed/retried recipientForgetMigration command after the state doc has been deleted.
recipientForgetMigrationThread.start();

// Block the recipient before it updates the state doc with an expireAt field.
fp.wait();

let currOp = assert
                 .commandWorked(recipientPrimary.adminCommand(
                     {currentOp: true, desc: "tenant recipient migration"}))
                 .inprog[0];
assert.eq(currOp.state, TenantMigrationTest.RecipientState.kDone, currOp);
assert(!currOp.hasOwnProperty("expireAt"), currOp);

// Test that we can still read from the recipient.
assert.eq(1, recipientPrimary.getDB(dbName)[collName].find().itcount());

const newRecipientPrimary = tenantMigrationTest.getRecipientRst().getSecondary();
const newPrimaryFp = configureFailPoint(newRecipientPrimary, "hangBeforeTaskCompletion");

// Step up a new recipient primary before the state doc is truly marked as garbage collectable.
tenantMigrationTest.getRecipientRst().stepUp(newRecipientPrimary);
fp.off();

// The new primary should skip all tenant migration steps but wait for another
// recipientForgetMigration command.
newPrimaryFp.wait();

assert.commandFailedWithCode(recipientForgetMigrationThread.returnData(),
                             ErrorCodes.InterruptedDueToReplStateChange);

// Test that we can still read from the recipient.
assert.eq(1, newRecipientPrimary.getDB(dbName)[collName].find().itcount());

// Test that we can retry the recipientForgetMigration on the new primary.
newPrimaryFp.off();
assert.commandWorked(runRecipientForgetMigration(newRecipientPrimary.host, {
    migrationIdString: extractUUIDFromObject(migrationId),
    donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
    tenantId,
    readPreference: {mode: "primary"},
}));

currOp = assert
             .commandWorked(newRecipientPrimary.adminCommand(
                 {currentOp: true, desc: "tenant recipient migration"}))
             .inprog[0];
assert.eq(currOp.state, TenantMigrationTest.RecipientState.kDone, currOp);
assert(currOp.hasOwnProperty("expireAt"), currOp);

tenantMigrationTest.stop();
