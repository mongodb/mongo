/**
 * Tests tenant migration with time-series collections.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_timeseries,
 *   serverless,
 *   # The validation hook in this suite enforces that all time-series buckets are compressed. This
 *   # will not be the case in multiversion suites.
 *   requires_fcv_71,
 *   # TODO (SERVER-80521): Re-enable this test once redness is resolve in multiversion suites.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const donorPrimary = tenantMigrationTest.getDonorPrimary();

const tenantId = ObjectId().str;
const tsDB = makeTenantDB(tenantId, "tsDB");
const collName = "tsColl";
const donorTSDB = donorPrimary.getDB(tsDB);
assert.commandWorked(donorTSDB.createCollection(collName, {timeseries: {timeField: "time"}}));
assert.commandWorked(donorTSDB.runCommand(
    {insert: collName, documents: [{_id: 1, time: ISODate()}, {_id: 2, time: ISODate()}]}));

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
};

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

const donorPrimaryCountDocumentsResult = donorTSDB[collName].countDocuments({});
const donorPrimaryCountResult = donorTSDB[collName].count();

// Creating a timeseries collection internally creates a view. Reading from timeseries collection
// works only if the view associated with the timeseries is present. So, this step ensures both the
// timeseries collection and the view are copied correctly to recipient.
tenantMigrationTest.getRecipientRst().nodes.forEach(node => {
    jsTestLog(`Checking ${tsDB}.${collName} on ${node}`);
    // Use "countDocuments" to check actual docs, "count" to check sizeStorer data.
    assert.eq(donorPrimaryCountDocumentsResult,
              node.getDB(tsDB)[collName].countDocuments({}),
              "countDocuments");
    assert.eq(donorPrimaryCountResult, node.getDB(tsDB)[collName].count(), "count");
});

tenantMigrationTest.stop();
