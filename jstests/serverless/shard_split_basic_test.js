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
    nodeOptions: {
        // Set a short timeout to test that the operation times out waiting for replication
        setParameter: "shardSplitTimeoutMS=100000"
    }
});

test.addRecipientNodes();
test.donor.awaitSecondaryNodes();

const migrationId = UUID();

jsTestLog("Running the commitShardSplit operation");
const admin = test.donor.getPrimary().getDB("admin");
assert.commandWorked(admin.runCommand({
    commitShardSplit: 1,
    migrationId,
    tenantIds: ["tenant1", "tenant2"],
    recipientTagName,
    recipientSetName
}));

jsTestLog("Forgetting shard split");
assert.commandWorked(test.donor.getPrimary().adminCommand({forgetShardSplit: 1, migrationId}));

test.stop();
})();
