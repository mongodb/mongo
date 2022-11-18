/**
 * Commits a shard split and shuts down while being in a blocking state. Tests that we recover the
 * tenant access blockers in blocking state with `blockOpTime` set.
 * @tags: [requires_fcv_62, serverless]
 */

load("jstests/libs/fail_point_util.js");                         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');                  // for "startParallelShell"
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // for "setParameter"
load("jstests/serverless/libs/shard_split_test.js");
load("jstests/replsets/libs/tenant_migration_test.js");

(function() {
"use strict";

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
const fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");

jsTestLog("Running Shard Split restart after blocking");
const tenantIds = [ObjectId(), ObjectId()];
const operation = test.createSplitOperation(tenantIds);
const splitThread = operation.commitAsync();

fp.wait();
assertMigrationState(donorPrimary, operation.migrationId, "blocking");

test.stop({shouldRestart: true});
splitThread.join();

test.donor.startSet({restart: true});

donorPrimary = test.donor.getPrimary();
assert(findSplitOperation(donorPrimary, operation.migrationId), "There must be a config document");

test.validateTenantAccessBlockers(
    operation.migrationId, tenantIds, TenantMigrationTest.DonorAccessState.kBlockWritesAndReads);

test.stop();
})();
