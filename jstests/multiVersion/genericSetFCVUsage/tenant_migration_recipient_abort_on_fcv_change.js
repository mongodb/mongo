/**
 * Tests that the recipient cancels all migrations when its FCV changes.
 * @tags: [
 *   requires_majority_read_concern,
 *   incompatible_with_windows_tls,
 *   serverless,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";
import {setLogVerbosity} from "jstests/replsets/rslib.js";

if (!buildInfo()["modules"].includes("enterprise")) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const tenantId = ObjectId().str;
const dbName = makeTenantDB(tenantId, "testDB");
const collName = "testColl";

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const donorDB = donorPrimary.getDB(dbName);

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const recipientDB = recipientPrimary.getDB(dbName);

setLogVerbosity([recipientPrimary], {"tenantMigration": {"verbosity": 3}});

tenantMigrationTest.insertDonorDB(dbName, collName);

const migrationId = UUID();
const migrationIdString = extractUUIDFromObject(migrationId);
const migrationOpts = {
    migrationIdString: migrationIdString,
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: tenantId,
};

const hangWhileMigratingRecipientFP = configureFailPoint(
    recipientDB, "fpAfterDataConsistentMigrationRecipientInstance", {action: "hang"});
const hangWhileMigratingDonorFP =
    configureFailPoint(donorDB, "pauseTenantMigrationBeforeLeavingDataSyncState");

// Start a migration and wait for donor to hang at the failpoint.
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

hangWhileMigratingDonorFP.wait();
hangWhileMigratingRecipientFP.wait();

// Initiate a downgrade and let it complete.
assert.commandWorked(recipientPrimary.adminCommand(
    {setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}));

// Upgrade again and finish the test.
assert.commandWorked(
    recipientPrimary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

hangWhileMigratingDonorFP.off();
hangWhileMigratingRecipientFP.off();

const stateRes =
    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.eq(stateRes.state, TenantMigrationTest.DonorState.kAborted);
assert.eq(stateRes.abortReason.code, ErrorCodes.TenantMigrationAborted);

tenantMigrationTest.waitForDonorNodesToReachState(
    donorRst.nodes, migrationId, tenantId, TenantMigrationTest.DonorState.kAborted);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
