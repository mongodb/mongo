/**
 * Checks that set/getClusterParameter omit test-only parameters when enableTestCommands
 * is false.
 *
 * @tags: [
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
// If feature flag enabling standalone cluster parameters is enabled, test on standalone.
const mongo = MongoRunner.runMongod({});
if (FeatureFlagUtil.isEnabled(mongo.getDB('admin'), 'AuditConfigClusterParameter')) {
    setupNode(mongo);
    // Assert that turning off enableTestCommands prevents test-only cluster server parameters
    // from being directly set/retrieved and filters them from the output of
    // getClusterParameter: '*'.
    testDisabledClusterParameters(mongo);
}
MongoRunner.stopMongod(mongo);

const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet();
rst.initiate();

// Setup the necessary logging level for the test.
setupReplicaSet(rst);
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
