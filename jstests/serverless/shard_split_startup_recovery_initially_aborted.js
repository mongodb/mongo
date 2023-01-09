/**
 * Starts a shard split through `abortShardSplit` and assert that no tenant access blockers are
 * recovered since we do not recover access blockers for aborted split marked garbage collectable.
 * Also verifies the serverless operation lock is not acquired when starting a split in aborted
 * state.
 * @tags: [requires_fcv_62, serverless]
 */

import {
    getServerlessOperationLock,
    ServerlessLockType
} from "jstests/replsets/libs/tenant_migration_util.js";
import {
    assertMigrationState,
    findSplitOperation,
    ShardSplitTest
} from "jstests/serverless/libs/shard_split_test.js";
load("jstests/libs/fail_point_util.js");  // for "configureFailPoint"

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const test = new ShardSplitTest({
    quickGarbageCollection: true,
    nodeOptions: {
        setParameter:
            {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"})}
    }
});
test.addRecipientNodes();

let donorPrimary = test.donor.getPrimary();
const migrationId = UUID();

assert.isnull(findSplitOperation(donorPrimary, migrationId));
// Pause the shard split before waiting to mark the doc for garbage collection.
let fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterDecision");

const tenantIds = [ObjectId(), ObjectId()];

assert.commandWorked(donorPrimary.adminCommand({abortShardSplit: 1, migrationId}));

fp.wait();

assertMigrationState(donorPrimary, migrationId, "aborted");

jsTestLog("Stopping the set");
test.stop({shouldRestart: true});

jsTestLog("Restarting the set");
test.donor.startSet({restart: true});

donorPrimary = test.donor.getPrimary();
assert(findSplitOperation(donorPrimary, migrationId), "There must be a config document");

// we do not recover access blockers for kAborted marked for garbage collection
tenantIds.every(tenantId => {
    assert.isnull(
        ShardSplitTest.getTenantMigrationAccessBlocker({node: donorPrimary, tenantId: tenantId}));
});

// We do not acquire the lock for document marked for garbage collection
assert.eq(getServerlessOperationLock(donorPrimary), ServerlessLockType.None);

test.stop();
