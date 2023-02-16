/**
 * Tests that a shard split handles cluster parameters.
 * @tags: [requires_fcv_63, serverless]
 */

import {assertMigrationState, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";
load("jstests/libs/cluster_server_parameter_utils.js");

const tenantIds = [ObjectId(), ObjectId()];

const nodeOptions = {
    setParameter: {multitenancySupport: true}
};

const test = new ShardSplitTest({quickGarbageCollection: true, nodeOptions});

test.addRecipientNodes({nodeOptions});
test.donor.awaitSecondaryNodes();

const donorPrimary = test.getDonorPrimary();

// Set a cluster parameter before the split starts.
assert.commandWorked(donorPrimary.getDB("admin").runCommand(tenantCommand(
    {setClusterParameter: {"changeStreams": {"expireAfterSeconds": 7200}}}, tenantIds[0])));

const operation = test.createSplitOperation(tenantIds);
assert.commandWorked(operation.commit());
assertMigrationState(donorPrimary, operation.migrationId, "committed");

operation.forget();

const recipientRst = test.getRecipient();

const {clusterParameters} =
    assert.commandWorked(recipientRst.getPrimary().getDB("admin").runCommand(
        tenantCommand({getClusterParameter: ["changeStreams"]}, tenantIds[0])));
const [changeStreamsClusterParameter] = clusterParameters;
assert.eq(changeStreamsClusterParameter.expireAfterSeconds, 7200);

test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);
test.stop();
