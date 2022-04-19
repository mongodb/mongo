'use strict';

(function() {
load('jstests/libs/check_routing_table_consistency_helpers.js');  // For check implementation.
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type !== Topology.kShardedCluster) {
    throw new Error(
        'Routing table consistency check must be run against a sharded cluster, but got: ' +
        tojson(topology));
}
RoutingTableConsistencyChecker.run(db.getMongo());
})();
