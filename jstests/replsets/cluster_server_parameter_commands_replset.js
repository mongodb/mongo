/**
 * Checks that getClusterParameter runs as expected on replica set nodes.
 *
 * @tags: [
 *   # Requires all nodes to be running the latest binary.
 *   requires_fcv_60,
 *   featureFlagClusterWideConfig,
 *   does_not_support_stepdowns
 *  ]
 */
(function() {
'use strict';

load('jstests/libs/cluster_server_parameter_utils.js');

// Tests that getClusterParameter works on a non-sharded replica set.
function runReplSetTest() {
    const rst = new ReplSetTest({
        nodes: 3,
    });
    rst.startSet();
    rst.initiate();

    // Setup the necessary logging level for the test.
    setupReplicaSet(rst);

    // First, ensure that nonexistent parameters and unauthorized users are rejected with the
    // appropriate error codes.
    testInvalidParameters(rst);

    // Then, ensure that getClusterParameter returns the expected values for all valid invocations
    // of getClusterParameter.
    testValidParameters(rst);

    rst.stopSet();
}

runReplSetTest();
})();
