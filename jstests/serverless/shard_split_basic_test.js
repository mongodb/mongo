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
const tenantIds = ["tenant1", "tenant2"];

const test =
    new BasicServerlessTest({recipientTagName, recipientSetName, quickGarbageCollection: true});

test.addRecipientNodes();
test.donor.awaitSecondaryNodes();

const operation = test.createSplitOperation(tenantIds);

jsTestLog("Running the commitShardSplit operation");
assert.commandWorked(operation.commit());

test.removeRecipientNodesFromDonor();

// getPrimary can only be called once recipient nodes have been remove from test.
assertMigrationState(test.donor.getPrimary(), operation.migrationId, "committed");

operation.forget();

test.waitForGarbageCollection(operation.migrationId, tenantIds);

test.stop();
})();
