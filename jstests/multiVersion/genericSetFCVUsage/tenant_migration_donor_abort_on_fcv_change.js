/**
 * Tests that the donor cancels all migrations when its FCV changes.
 * @tags: [
 *   requires_majority_read_concern,
 *   incompatible_with_windows_tls,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/replsets/rslib.js");       // for 'setLogVerbosity'

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const tenantId = ObjectId().str;
const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
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
    donorPrimary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));

// Upgrade again and finish the test.
assert.commandWorked(donorPrimary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

hangWhileMigratingFP.off();

TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                  ErrorCodes.TenantMigrationAborted);

tenantMigrationTest.waitForDonorNodesToReachState(
    donorRst.nodes, migrationId, tenantId, TenantMigrationTest.DonorState.kAborted);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
