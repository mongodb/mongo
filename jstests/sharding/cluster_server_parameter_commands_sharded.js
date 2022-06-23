/**
 * Checks that set/getClusterParameter runs as expected on sharded clusters.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_sharding,
 *   multiversion_incompatible
 *  ]
 */
(function() {
'use strict';

load('jstests/libs/cluster_server_parameter_utils.js');

// Tests that set/getClusterParameter works on all nodes of a sharded cluster.
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

// First, ensure that incorrect usages of set/getClusterParameter fail appropriately on mongos
// and cluster mongods.
testInvalidClusterParameterCommands(st);

// Then, ensure that set/getClusterParameter set and retrieve the expected values on mongos
// and the majority of nodes on all replica sets in the cluster.
testValidClusterParameterCommands(st);

st.stop();
})();
