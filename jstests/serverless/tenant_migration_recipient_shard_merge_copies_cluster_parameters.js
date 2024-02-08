/**
 * Tests that recipient is able to copy and apply cluster parameters from the donor for the shard
 * merge protocol.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 *   requires_shard_merge,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    makeUnsignedSecurityToken,
    runCommandWithSecurityToken
} from "jstests/libs/multitenancy_utils.js"
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    quickGarbageCollection: true,
    sharedOptions: {
        setParameter: {
            multitenancySupport: true,
        },
    },
});

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = ObjectId();
const tenantToken = makeUnsignedSecurityToken(tenantId, {expectPrefix: false});

// Set a cluster parameter before the migration starts.
assert.commandWorked(runCommandWithSecurityToken(tenantToken, donorPrimary.getDB("admin"), {
    setClusterParameter: {"changeStreams": {"expireAfterSeconds": 7200}}
}));

const fpBeforeMarkingCloneSuccess =
    configureFailPoint(recipientPrimary, "fpBeforeMarkingCloneSuccess", {action: "hang"});

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: "primary"},
    tenantIds: [tenantId],
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

fpBeforeMarkingCloneSuccess.wait();

// Set another cluster parameter so that oplog entries are applied during oplog catchup.
assert.commandWorked(runCommandWithSecurityToken(tenantToken, donorPrimary.getDB("admin"), {
    setClusterParameter: {"testStrClusterParameter": {"strData": "sleep"}}
}));

fpBeforeMarkingCloneSuccess.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

const {clusterParameters} = assert.commandWorked(runCommandWithSecurityToken(
    tenantToken,
    recipientPrimary.getDB("admin"),
    {getClusterParameter: ["changeStreams", "testStrClusterParameter"]}));
const [changeStreamsClusterParameter, testStrClusterParameter] = clusterParameters;
assert.eq(changeStreamsClusterParameter.expireAfterSeconds, 7200);
assert.eq(testStrClusterParameter.strData, "sleep");

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);

tenantMigrationTest.stop();
