/**
 * Tests that donor is able to implicitly abort the migration upon recipient primary restart.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_shard_merge,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB, makeX509OptionsForTest} from "jstests/replsets/libs/tenant_migration_util.js";

const recipientRst = new ReplSetTest({
    nodes: 1,
    name: "recipient",
    serverless: true,
    nodeOptions: Object.assign(makeX509OptionsForTest().recipient,
                               {setParameter: {tenantMigrationGarbageCollectionDelayMS: 0}})
});
recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    quickGarbageCollection: true,
    sharedOptions: {nodes: 1},
    recipientRst: recipientRst
});
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = recipientRst.getPrimary();

const tenantId = ObjectId();
const tenantDB = makeTenantDB(tenantId.str, "DB");
const collName = "testColl";

// Do a majority write.
tenantMigrationTest.insertDonorDB(tenantDB, collName);

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: 'primary'},
    tenantIds: [tenantId],
};

const fpBeforePersistingRejectReadsBeforeTimestamp = configureFailPoint(
    recipientPrimary, "fpBeforePersistingRejectReadsBeforeTimestamp", {action: "hang"});

jsTestLog("Start the Migration");
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait for migration to hang on the recipient side.
fpBeforePersistingRejectReadsBeforeTimestamp.wait();

jsTestLog("Shutdown recipient primary");
recipientRst.stopSet(
    null /* signal */, true /* forRestart */, {noCleanData: true, skipValidation: true});

jsTestLog("Wait for the migration to abort");
const migrationRes = TenantMigrationTest.assertAborted(
    tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.includes([ErrorCodes.TenantMigrationAborted, ErrorCodes.HostUnreachable],
                migrationRes.abortReason.code,
                tojson(migrationRes));

jsTestLog("Restart recipient primary");
recipientRst.startSet({restart: true});
recipientRst.getPrimary();

jsTestLog("Forget Migration");
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
recipientRst.stopSet();
