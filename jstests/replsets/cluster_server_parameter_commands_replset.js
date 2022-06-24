/**
 * Checks that set/getClusterParameter runs as expected on replica set nodes.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   multiversion_incompatible
 *  ]
 */
(function() {
'use strict';

load('jstests/libs/cluster_server_parameter_utils.js');

// Tests that set/getClusterParameter works on a non-sharded replica set.
const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet();
rst.initiate();

// Setup the necessary logging level for the test.
setupReplicaSet(rst);

// First, ensure that incorrect usages of set/getClusterParameter fail appropriately.
testInvalidClusterParameterCommands(rst);

// Then, ensure that set/getClusterParameter set and retrieve the expected values on the
// majority of the nodes in the replica set.
testValidClusterParameterCommands(rst);

rst.stopSet();
})();
