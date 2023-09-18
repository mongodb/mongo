/**
 * Tests recipient's ability to interrupt migration at various points during physical cloning phase
 * and perform cleanup upon abort.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_shard_merge,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {forgetMigrationAsync, makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";
import {createRstArgs} from "jstests/replsets/rslib.js";

const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), quickGarbageCollection: true, sharedOptions: {nodes: 3}});

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientRst = tenantMigrationTest.getRecipientRst();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const recipientSecondary = tenantMigrationTest.getRecipientRst().getSecondaries()[0];
const donorRstArgs = createRstArgs(tenantMigrationTest.getDonorRst());

const kTenantCount = 4;
const tenantIds = [...Array(kTenantCount).keys()].map(() => ObjectId());
const dbNames = ["db0", "db1", "db2"];
const collNames = ["coll0", "coll1"];

function forEachTenantCollRun(workFn) {
    tenantIds.forEach((tenantId) => {
        dbNames.forEach((dbName) => {
            const tenantDB = makeTenantDB(tenantId.str, dbName);
            collNames.forEach((collName) => {
                jsTestLog(`Executing work for tenant database: ${tenantDB}.${collName}`);
                workFn(tenantDB, collName);
            });
        });
    });
}

// Load tenant data on donor.
forEachTenantCollRun((tenantDB, collName) => tenantMigrationTest.insertDonorDB(tenantDB, collName));

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: 'primary'},
    tenantIds: tenantIds,
};

// To test the recipient's ability to abort migration at various points during physical cloning
// phase, we enable different failpoints on each recipient node.
const waitInFailPointOnRPrimary =
    configureFailPoint(recipientPrimary, "TenantFileClonerHangDuringFileCloneBackup");
const waitInFailPointOnRSec = configureFailPoint(recipientSecondary, "hangBeforeImportingFiles");

jsTestLog("Start migration");
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait for the failpoints on recipient nodes.
waitInFailPointOnRPrimary.wait();
waitInFailPointOnRSec.wait();

jsTestLog("Abort migration");
assert.commandWorked(tenantMigrationTest.tryAbortMigration(migrationOpts));

const forgetMigrationThread = new Thread(async (migrationOpts, donorRstArgs) => {
    const {forgetMigrationAsync} = await import("jstests/replsets/libs/tenant_migration_util.js");
    // Sleep for some random time.
    const kMaxSleepTimeMS = 350;
    sleep(Math.random() * kMaxSleepTimeMS);
    jsTestLog("Forget migration");
    assert.commandWorked(await forgetMigrationAsync(migrationOpts.migrationIdString, donorRstArgs));
}, migrationOpts, donorRstArgs);
forgetMigrationThread.start();

// Disable the failpoint to unblock migration.
waitInFailPointOnRPrimary.off();
waitInFailPointOnRSec.off();

forgetMigrationThread.join();

// Wait for abort oplog entry to replicate on all recipient nodes before performing below sanity
// check.
recipientRst.awaitReplication();

const importMarkerCollName = "importDoneMarker." + extractUUIDFromObject(migrationUuid);
const tempWTPath = "/migrationTmpFiles." + extractUUIDFromObject(migrationUuid);

recipientRst.nodes.forEach(node => {
    jsTestLog(`Checking ${node} for temp files.`);
    assert(!pathExists(node.dbpath + tempWTPath),
           "Temp WiredTiger directory should not exist, but present");
    assert.eq(0, node.getDB("local").getCollectionInfos({name: importMarkerCollName}).length);
    forEachTenantCollRun((tenantDB, collName) => assert.eq(
                             0, node.getDB(tenantDB).getCollectionInfos({name: collName}).length));
});

// Wait for the state doc to be deleted before retrying with the same migrationId.
tenantMigrationTest.waitForMigrationGarbageCollection(migrationUuid, tenantIds[0]);

jsTestLog("Retry migration");
TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

tenantMigrationTest.stop();
