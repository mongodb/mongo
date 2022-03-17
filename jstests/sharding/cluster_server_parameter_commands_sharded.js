/**
 * Checks that getClusterParameter runs as expected on sharded clusters.
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

// Tests that getClusterParameter works on all nodes of a sharded cluster.
function runShardedTest() {
    const options = {
        mongos: 1,
        config: 1,
        shards: 3,
        rs: {
            nodes: 3,
        },
    };
    const st = new ShardingTest(options);

    // Setup the necessary logging on mongos and the shards.
    setupSharded(st);

    // First, ensure that nonexistent parameters are rejected with the
    // appropriate error codes on mongos and all shards.
    testInvalidParameters(st);

    // Then, ensure that getClusterParameter returns the expected values for all valid invocations
    // of getClusterParameter.
    testValidParameters(st);

    st.stop();
}

runShardedTest();
})();
