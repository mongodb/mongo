/**
 * Tests that a recipientForgetMigration is received after the recipient state doc has been deleted
 * for shard merge protocol.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_shard_merge,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {kProtocolShardMerge, makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const migrationId = UUID();
const tenantId = ObjectId();

const dbName = makeTenantDB(tenantId.str, "test");
const collName = "coll";

// Not doing a migration before writing to the recipient to mimic that a migration has completed and
// the state doc has been garbage collected.
assert.commandWorked(recipientPrimary.getDB(dbName)[collName].insert({_id: 1}));

function runRecipientForgetMigration(
    host, {migrationIdString, donorConnectionString, tenantIds}, protocol) {
    const db = new Mongo(host);
    return db.adminCommand({
        recipientForgetMigration: 1,
        migrationId: UUID(migrationIdString),
        donorConnectionString,
        tenantIds: eval(tenantIds),
        protocol,
        decision: "committed",
        readPreference: {mode: "primary"},
    });
}

const fp = configureFailPoint(
    recipientPrimary, "fpBeforeMarkingStateDocAsGarbageCollectable", {action: "hang"});

const recipientForgetMigrationThread =
    new Thread(runRecipientForgetMigration,
               recipientPrimary.host,
               {
                   migrationIdString: extractUUIDFromObject(migrationId),
                   donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
                   tenantIds: tojson([tenantId])
               },
               kProtocolShardMerge);

// Run a delayed/retried recipientForgetMigration command after the state doc has been deleted.
recipientForgetMigrationThread.start();

// Block the recipient before it updates the state doc with an expireAt field.
fp.wait();

let currOp = assert
                 .commandWorked(recipientPrimary.adminCommand(
                     {currentOp: true, desc: "shard merge recipient"}))
                 .inprog[0];
assert.eq(currOp.state, TenantMigrationTest.ShardMergeRecipientState.kCommitted, currOp);
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
assert.commandWorked(runRecipientForgetMigration(
    newRecipientPrimary.host,
    {
        migrationIdString: extractUUIDFromObject(migrationId),
        donorConnectionString: tenantMigrationTest.getDonorRst().getURL(),
        tenantIds: tojson([tenantId]),
    },
    kProtocolShardMerge));

currOp = assert
             .commandWorked(
                 newRecipientPrimary.adminCommand({currentOp: true, desc: "shard merge recipient"}))
             .inprog[0];
assert.eq(currOp.state, TenantMigrationTest.ShardMergeRecipientState.kCommitted, currOp);
assert(currOp.hasOwnProperty("expireAt"), currOp);

tenantMigrationTest.stop();
