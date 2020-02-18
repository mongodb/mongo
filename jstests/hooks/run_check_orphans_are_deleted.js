'use strict';

/**
 * Asserts that no shard in the cluster contains any orphan documents.
 *
 * Note: This hook won't find documents which don't have the full shard key.
 */
(function() {
load('jstests/libs/check_orphans_are_deleted_helpers.js');  // For CheckOrphansAreDeletedHelpers.
load('jstests/libs/discover_topology.js');                  // For Topology and DiscoverTopology.

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type !== Topology.kShardedCluster) {
    throw new Error('Orphan documents check must be run against a sharded cluster, but got: ' +
                    tojson(topology));
}

for (let shardName of Object.keys(topology.shards)) {
    const shard = topology.shards[shardName];
    let shardPrimary;

    if (shard.type === Topology.kStandalone) {
        shardPrimary = shard.mongod;
    } else if (shard.type === Topology.kReplicaSet) {
        shardPrimary = shard.primary;
    } else {
        throw new Error('Unrecognized topology format: ' + tojson(topology));
    }

    CheckOrphansAreDeletedHelpers.runCheck(db.getMongo(), new Mongo(shardPrimary), shardName);
}
})();
