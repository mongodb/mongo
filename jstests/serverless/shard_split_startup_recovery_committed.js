/**
 * Commits a shard split and shut down prior to marking the state document as garbage collectable.
 * Checks that we recover the tenant access blockers with `commitOpTime` and `blockOpTime` set.
 * @tags: [requires_fcv_62, serverless]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
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

// Pause the shard split before waiting to mark the doc for garbage collection.
let fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterDecision");

jsTestLog("Running Shard Split restart after committed");
const tenantIds = [ObjectId(), ObjectId()];
const operation = test.createSplitOperation(tenantIds);
const splitThread = operation.commitAsync();
fp.wait();

splitThread.join();
assertMigrationState(donorPrimary, operation.migrationId, "committed");

test.stop({shouldRestart: true});

test.donor.startSet({restart: true});

donorPrimary = test.donor.getPrimary();
const splitDoc = findSplitOperation(donorPrimary, operation.migrationId);
assert(splitDoc, "There must be a config document");

test.validateTenantAccessBlockers(
    operation.migrationId, tenantIds, TenantMigrationTest.DonorAccessState.kReject);

test.stop();
