'use strict';

(function() {
load('jstests/libs/check_metadata_consistency_helpers.js');  // For check implementation.
load('jstests/libs/discover_topology.js');                   // For DiscoverTopology and Topology.

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const mongos = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

if (topology.type !== Topology.kShardedCluster) {
    throw new Error('Metadata consistency check must be run against a sharded cluster, but got: ' +
                    tojson(topology));
}

MetadataConsistencyChecker.run(mongos);
})();
