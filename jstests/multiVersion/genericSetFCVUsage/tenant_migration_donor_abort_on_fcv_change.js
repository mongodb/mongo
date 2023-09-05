/**
 * Tests that the donor cancels all migrations when its FCV changes.
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

setLogVerbosity([donorPrimary], {"tenantMigration": {"verbosity": 3}});

tenantMigrationTest.insertDonorDB(dbName, collName);

const migrationId = UUID();
const migrationIdString = extractUUIDFromObject(migrationId);
const migrationOpts = {
    migrationIdString: migrationIdString,
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: tenantId,
};

const hangWhileMigratingFP =
    configureFailPoint(donorDB, "pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState");

// Start a migration and wait for donor to hang at the failpoint.
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

hangWhileMigratingFP.wait();

// Initiate a downgrade and let it complete.
assert.commandWorked(
    donorPrimary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}));

// Upgrade again and finish the test.
assert.commandWorked(
    donorPrimary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

hangWhileMigratingFP.off();

TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                  ErrorCodes.TenantMigrationAborted);

tenantMigrationTest.waitForDonorNodesToReachState(
    donorRst.nodes, migrationId, tenantId, TenantMigrationTest.DonorState.kAborted);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
