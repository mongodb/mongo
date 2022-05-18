/**
 *
 * Tests that runs a shard split to completion.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

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
assert.commandWorked(operation.commit());

assertMigrationState(test.getDonorPrimary(), operation.migrationId, "committed");

operation.forget();

test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);

test.stop();
})();
