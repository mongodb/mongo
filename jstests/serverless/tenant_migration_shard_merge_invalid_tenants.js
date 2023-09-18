/**
 * Tests that shard merge fails if the list of tenants passed to donorStartMigration is incomplete.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_shard_merge,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

// Set a shorter importQuorumTimeoutSeconds for automatic merge abort.
const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    sharedOptions: {nodes: 1, setParameter: {importQuorumTimeoutSeconds: 60}}
});

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = ObjectId();
const tenantDB = makeTenantDB(tenantId.str, "DB");
const collName = "testColl";

const invalidTenantId = ObjectId();
const invalidTenantDB = makeTenantDB(invalidTenantId.str, "DB");
const invalidCollName = "problemColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();

donorPrimary.getDB(tenantDB)[collName].insertOne({name: "dummyDoc"});
donorPrimary.getDB(invalidTenantDB)[invalidCollName].insertOne({name: "problemDoc"});

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: 'primary'},
    tenantIds: [tenantId]
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

checkLog.contains(recipientPrimary, new RegExp(`6615001.*InvalidTenantId`));

TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts),
                                  ErrorCodes.ExceededTimeLimit);

tenantMigrationTest.stop();
