/**
 *
 * Tests that runs a shard split to completion.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');  // for "startParallelShell"
load("jstests/serverless/libs/basic_serverless_test.js");

(function() {
"use strict";

const recipientTagName = "recipientNode";
const recipientSetName = "recipientSetName";
const test = new BasicServerlessTest({
    recipientTagName,
    recipientSetName,
    quickGarbageCollection: true,
    nodeOptions: {
        setParameter:  // Timeout to test that the operation times out waiting for replication
            {shardSplitTimeoutMS: 2000}
    }
});

test.addRecipientNodes();
test.donor.awaitSecondaryNodes();

const migrationId = UUID();

jsTestLog("Running the commitShardSplit operation");
const admin = test.donor.getPrimary().getDB("admin");
const tenantIds = ["tenant1", "tenant2"];
assert.commandWorked(admin.runCommand(
    {commitShardSplit: 1, migrationId, tenantIds, recipientTagName, recipientSetName}));

test.removeRecipientNodesFromDonor();

test.forgetShardSplit(migrationId);

test.waitForGarbageCollection(migrationId, tenantIds);
test.stop();
})();
