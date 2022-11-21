"use strict";

(function(keyFile) {
load("jstests/libs/check_cluster_index_consistency_helpers.js");  // For check implementation.
load("jstests/libs/discover_topology.js");  // For Topology and DiscoverTopology.

assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a server?");

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type !== Topology.kShardedCluster) {
    throw new Error(
        "Cluster index consistency check must be run against a sharded cluster, but got: " +
        tojson(topology));
}
ClusterIndexConsistencyChecker.run(db.getMongo(), keyFile);
})(this.keyFile);
