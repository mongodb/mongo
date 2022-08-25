/**
 * Commits a shard split and shuts down while being in a blocking state. Tests that we recover the
 * tenant access blockers in blocking state with `blockOpTime` set.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");                         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');                  // for "startParallelShell"
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // for "setParameter"
load("jstests/serverless/libs/basic_serverless_test.js");
load("jstests/replsets/libs/tenant_migration_test.js");

(function() {
"use strict";

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const recipientTagName = "recipientNode";
const recipientSetName = "recipientSetName";

const test = new BasicServerlessTest({
    recipientTagName,
    recipientSetName,
    quickGarbageCollection: true,
    nodeOptions: {
        setParameter:
            {"failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"})}
    }
});
test.addRecipientNodes();

const migrationId = UUID();
let donorPrimary = test.donor.getPrimary();
assert.isnull(findSplitOperation(donorPrimary, migrationId));

let fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");

jsTestLog("Running Shard Split restart after blocking");
const tenantIds = ["tenant1", "tenant2"];
const awaitFirstSplitOperation = startParallelShell(
    funWithArgs(function(migrationId, recipientTagName, recipientSetName, tenantIds) {
        db.adminCommand(
            {commitShardSplit: 1, migrationId, recipientTagName, recipientSetName, tenantIds});
    }, migrationId, recipientTagName, recipientSetName, tenantIds), donorPrimary.port);

fp.wait();

assertMigrationState(donorPrimary, migrationId, "blocking");

test.stop({shouldRestart: true});
awaitFirstSplitOperation();

test.donor.startSet({restart: true});

donorPrimary = test.donor.getPrimary();
assert(findSplitOperation(donorPrimary, migrationId), "There must be a config document");

test.validateTenantAccessBlockers(
    migrationId, tenantIds, TenantMigrationTest.DonorAccessState.kBlockWritesAndReads);

test.stop();
})();
