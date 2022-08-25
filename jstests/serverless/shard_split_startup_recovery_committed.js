/**
 * Commits a shard split and shut down prior to marking the state document as garbage collectable.
 * Checks that we recover the tenant access blockers with `commitOpTime` and `blockOpTime` set.
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
const recipientSetName = "recipient";
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

// Pause the shard split before waiting to mark the doc for garbage collection.
let fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterDecision");

jsTestLog("Running Shard Split restart after committed");
const tenantIds = ["tenant3", "tenant4"];
assert.commandWorked(donorPrimary.adminCommand(
    {commitShardSplit: 1, migrationId, recipientTagName, recipientSetName, tenantIds}));

fp.wait();

assertMigrationState(donorPrimary, migrationId, "committed");

test.stop({shouldRestart: true});

test.donor.startSet({restart: true});

donorPrimary = test.donor.getPrimary();
const splitDoc = findSplitOperation(donorPrimary, migrationId);
assert(splitDoc, "There must be a config document");

test.validateTenantAccessBlockers(
    migrationId, tenantIds, TenantMigrationTest.DonorAccessState.kReject);

test.stop();
})();
