'use strict';

/**
 * Checks the filtering metadata on the shards matches the one in the configsvr.
 */
(function() {
load('jstests/libs/check_shard_filtering_metadata_helpers.js');
load('jstests/libs/discover_topology.js');

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type !== Topology.kShardedCluster) {
    throw new Error(
        'Filtering metadata check can only be run against a sharded cluster, but got: ' +
        tojson(topology));
}

for (let shardName of Object.keys(topology.shards)) {
    const shard = topology.shards[shardName];

    if (shard.type !== Topology.kReplicaSet) {
        throw new Error('Unexpected topology: ' + tojson(topology));
    }

    // Await replication to ensure that metadata on secondary nodes is up-to-date.
    new ReplSetTest(shard.nodes[0]).awaitSecondaryNodes();

    // Skipping checking sharded collection metadata because any workload on the suite could perform
    // an operation that is known to leave incorrect metadata (such as refineCollectionShardKey).
    shard.nodes.forEach(node => {
        CheckShardFilteringMetadataHelpers.run(
            db.getMongo(), new Mongo(node), shardName, true /* skipCheckShardedCollections */);
    });
}
})();
