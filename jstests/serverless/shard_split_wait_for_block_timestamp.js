/*
 * Test that the shard split operation waits for recipient nodes to reach the blockOpTime by
 * pausing replication and observing the operation time out, then reenabling replication and
 * observing a successful split.
 *
 * @tags: [requires_fcv_52, featureFlagShardSplit, serverless]
 */

load("jstests/libs/fail_point_util.js");                         // for "configureFailPoint"
load("jstests/libs/write_concern_util.js");                      // for "stopServerReplication"
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // for "setParameter"
load('jstests/libs/parallel_shell_helpers.js');                  // for "startParallelShell"
load("jstests/serverless/libs/basic_serverless_test.js");

(function() {
"use strict";

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;
const test = new BasicServerlessTest({
    recipientTagName: "recipientNode",
    recipientSetName: "recipient",
    quickGarbageCollection: true,
    nodeOptions: {
        setParameter:  // Timeout to test that the operation times out waiting for replication
            {shardSplitTimeoutMS: 2000}
    }
});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const tenantIds = ["tenant1", "tenant2"];

// Stop replication on recipient nodes, and write a lot of data to the set
test.recipientNodes.forEach(node => stopServerReplication(node));
const largeString = 'X'.repeat(10000);
const bulk = donorPrimary.getDB("foo").bar.initializeUnorderedBulkOp();
for (let i = 0; i < 2000; i++) {
    bulk.insert({i, big: largeString});
}
assert.commandWorked(bulk.execute());

jsTestLog("Running commitShardSplit command");
const firstOperation = test.createSplitOperation(tenantIds);
assert.commandFailedWithCode(firstOperation.commit({retryOnRetryableErrors: false}),
                             ErrorCodes.TenantMigrationAborted);

firstOperation.forget();
test.cleanupSuccesfulAborted(firstOperation.migrationId, tenantIds);

jsTestLog("Restarting replication on recipient nodes, and running new split operation");
test.addRecipientNodes();
test.recipientNodes.forEach(node => restartServerReplication(node));
test.donor.awaitReplication();
test.donor.nodes.forEach(
    node => assert.commandWorked(setParameter(node, "shardSplitTimeoutMS", 60 * 1000)));

const secondOperation = test.createSplitOperation(tenantIds);
assert.isnull(findSplitOperation(donorPrimary, secondOperation.migrationId));
assert.commandWorked(secondOperation.commit());
assertMigrationState(donorPrimary, secondOperation.migrationId, "committed");

secondOperation.forget();

test.waitForGarbageCollection(secondOperation.migrationId, tenantIds);
test.stop();
})();
