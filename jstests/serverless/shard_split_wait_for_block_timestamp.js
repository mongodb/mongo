/*
 * Test that the shard split operation waits for recipient nodes to reach the blockTimestamp by
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
    nodeOptions: {
        // Set a short timeout to test that the operation times out waiting for replication
        setParameter: "shardSplitTimeoutMS=2000"
    }
});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const adminDb = donorPrimary.getDB("admin");
const tenantIds = ["test_tenant_1", "test_tenant_2"];

// Stop replication on recipient nodes, and write a lot of data to the set
test.recipientNodes.forEach(node => stopServerReplication(node));
const largeString = 'X'.repeat(10000);
const bulk = donorPrimary.getDB("foo").bar.initializeUnorderedBulkOp();
for (let i = 0; i < 2000; i++) {
    bulk.insert({i, big: largeString});
}
assert.commandWorked(bulk.execute());

// TODO(SERVER-62346): remove this when we actually split recipients
configureFailPoint(adminDb, "skipShardSplitWaitForSplitAcceptance");

jsTestLog("Running commitShardSplit command");
const firstOperationId = UUID();
assert.isnull(findMigration(donorPrimary, firstOperationId));
const awaitFirstSplitOperation = startParallelShell(
    funWithArgs(
        function(migrationId, recipientTagName, recipientSetName, tenantIds) {
            assert.commandWorked(db.adminCommand(
                {commitShardSplit: 1, migrationId, recipientTagName, recipientSetName, tenantIds}));
        },
        firstOperationId,
        test.recipientTagName,
        test.recipientSetName,
        tenantIds),
    donorPrimary.port);

awaitFirstSplitOperation();
assertMigrationState(donorPrimary, firstOperationId, "aborted");

jsTestLog(`Running forgetShardSplit command: ${tojson(firstOperationId)}`);
assert.commandWorked(adminDb.runCommand({forgetShardSplit: 1, migrationId: firstOperationId}));

jsTestLog("Restarting replication on recipient nodes, and running new split operation");
test.recipientNodes.forEach(node => restartServerReplication(node));
test.donor.awaitReplication();
test.donor.nodes.forEach(
    node => assert.commandWorked(setParameter(node, "shardSplitTimeoutMS", 10000)));

const secondOperationId = UUID();
assert.isnull(findMigration(donorPrimary, secondOperationId));
const awaitSecondSplitOperation = startParallelShell(
    funWithArgs(
        function(migrationId, recipientTagName, recipientSetName, tenantIds) {
            assert.commandWorked(db.adminCommand(
                {commitShardSplit: 1, migrationId, recipientTagName, recipientSetName, tenantIds}));
        },
        secondOperationId,
        test.recipientTagName,
        test.recipientSetName,
        tenantIds),
    donorPrimary.port);

awaitSecondSplitOperation();
assertMigrationState(donorPrimary, secondOperationId, "committed");

jsTestLog(`Running forgetShardSplit command: ${tojson(secondOperationId)}`);
assert.commandWorked(adminDb.runCommand({forgetShardSplit: 1, migrationId: secondOperationId}));

test.stop();
})();
