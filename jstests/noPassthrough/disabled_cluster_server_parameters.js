/**
 * Checks that set/getClusterParameter omit test-only parameters when enableTestCommands
 * is false.
 *
 * @tags: [
 *   # Requires all nodes to be running the latest binary.
 *   requires_fcv_60,
 *   featureFlagClusterWideConfig,
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_sharding
 *  ]
 */
(function() {
'use strict';

load('jstests/libs/cluster_server_parameter_utils.js');

// Verifies that test-only parameters are disabled and excluded when enableTestCommands is false.
TestData.enableTestCommands = false;
const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet();
rst.initiate();

// Setup the necessary logging level for the test.
setupReplicaSet(rst);

// Assert that turning off enableTestCommands prevents test-only cluster server parameters
// from being directly set/retrieved and filters them from the output of
// getClusterParameter: '*'.
testDisabledClusterParameters(rst);
rst.stopSet();

// Repeat the same on a sharded cluster.
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

// Check that the same behavior for disabled cluster server parameters holds on sharded clusters.
testDisabledClusterParameters(st);
st.stop();
}());
