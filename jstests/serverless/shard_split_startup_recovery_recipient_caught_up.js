/**
 * Commits a shard split and shuts down while being in a "recipient caught up" state. Tests that we
 * recover the tenant access blockers in blocking state with `blockOpTime` set.
 * @tags: [requires_fcv_71, serverless]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    assertMigrationState,
    findSplitOperation,
    ShardSplitTest
} from "jstests/serverless/libs/shard_split_test.js";

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
const fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterRecipientCaughtUp");

jsTestLog("Running Shard Split restart after recipient caught up");
const tenantIds = [ObjectId(), ObjectId()];
const operation = test.createSplitOperation(tenantIds);
const splitThread = operation.commitAsync();

fp.wait();
assertMigrationState(donorPrimary, operation.migrationId, "recipient caught up");

test.stop({shouldRestart: true});
splitThread.join();

test.donor.startSet({restart: true});

donorPrimary = test.donor.getPrimary();
assert(findSplitOperation(donorPrimary, operation.migrationId), "There must be a config document");

test.validateTenantAccessBlockers(
    operation.migrationId, tenantIds, TenantMigrationTest.DonorAccessState.kBlockWritesAndReads);

test.stop();
