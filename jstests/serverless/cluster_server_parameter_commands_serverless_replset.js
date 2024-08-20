/**
 * Checks that set/getClusterParameter runs as expected on serverless replica set nodes.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   # SERVER-90248 fixed in 8.1
 *   requires_fcv_81,
 *   serverless
 *  ]
 */
import {
    setupReplicaSet,
    testGetClusterParameterStar,
    testInvalidClusterParameterCommands,
    testValidServerlessClusterParameterCommands,
} from "jstests/libs/cluster_server_parameter_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Tests that set/getClusterParameter works on a non-sharded replica set.
const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet({
    setParameter: {
        multitenancySupport: true,
        featureFlagRequireTenantID: true,
        featureFlagServerlessChangeStreams: true
    }
});
rst.initiate();

// Setup the necessary logging level for the test.
setupReplicaSet(rst);

// First, ensure that incorrect usages of set/getClusterParameter fail appropriately.
for (const tenantId of [undefined, ObjectId()]) {
    testInvalidClusterParameterCommands(rst, tenantId);
}

// Then, ensure that set/getClusterParameter set and retrieve the expected values on the
// majority of the nodes in the replica set.
testValidServerlessClusterParameterCommands(rst);

// Ensure that getClusterParameter: "*" works as expected.
for (const tenantId of [undefined, ObjectId()]) {
    testGetClusterParameterStar(rst, tenantId);
}

rst.stopSet();
