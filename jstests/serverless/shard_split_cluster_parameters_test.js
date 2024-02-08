/**
 * Tests that a shard split handles cluster parameters.
 * @tags: [requires_fcv_63, serverless]
 */

import {
    makeUnsignedSecurityToken,
    runCommandWithSecurityToken
} from "jstests/libs/multitenancy_utils.js"
import {assertMigrationState, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

const tenantIds = [ObjectId(), ObjectId()];

const nodeOptions = {
    setParameter: {multitenancySupport: true}
};

const test = new ShardSplitTest({quickGarbageCollection: true, nodeOptions});

test.addRecipientNodes({nodeOptions});
test.donor.awaitSecondaryNodes();

const donorPrimary = test.getDonorPrimary();
const tenantToken0 = makeUnsignedSecurityToken(tenantIds[0], {expectPrefix: false});

// Set a cluster parameter before the split starts.
assert.commandWorked(runCommandWithSecurityToken(tenantToken0, donorPrimary.getDB("admin"), {
    setClusterParameter: {"changeStreams": {"expireAfterSeconds": 7200}}
}));

const operation = test.createSplitOperation(tenantIds);
assert.commandWorked(operation.commit());
assertMigrationState(donorPrimary, operation.migrationId, "committed");

operation.forget();

const recipientRst = test.getRecipient();

const {clusterParameters} =
    assert.commandWorked(runCommandWithSecurityToken(tenantToken0,
                                                     recipientRst.getPrimary().getDB("admin"),
                                                     {getClusterParameter: ["changeStreams"]}));
const [changeStreamsClusterParameter] = clusterParameters;
assert.eq(changeStreamsClusterParameter.expireAfterSeconds, 7200);

test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);
test.stop();
