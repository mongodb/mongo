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

const donorPrimary = test.getDonorPrimary();
const operation = test.createSplitOperation(tenantIds);
assert.commandWorked(operation.commit());
assertMigrationState(donorPrimary, operation.migrationId, "committed");
operation.forget();

const status = donorPrimary.adminCommand({serverStatus: 1});
assert.eq(status.shardSplits.totalCommitted, 1);
assert.eq(status.shardSplits.totalAborted, 0);
assert.gt(status.shardSplits.totalCommittedDurationMillis, 0);
assert.gt(status.shardSplits.totalCommittedDurationWithoutCatchupMillis, 0);

const recipientPrimary = test.getRecipient().getPrimary();
const recipientConfig = recipientPrimary.adminCommand({replSetGetConfig: 1}).config;
assert(!recipientConfig.settings.shardSplitBlockOpTime);

test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);
test.stop();
})();
