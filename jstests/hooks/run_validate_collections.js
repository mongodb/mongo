// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    load('jstests/libs/discover_topology.js');      // For Topology and DiscoverTopology.
    load('jstests/hooks/validate_collections.js');  // For CollectionValidator.

    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

    const hostList = [];

    if (topology.type === Topology.kStandalone) {
        hostList.push(topology.mongod);
    } else if (topology.type === Topology.kReplicaSet) {
        hostList.push(...topology.nodes);
    } else if (topology.type === Topology.kShardedCluster) {
        hostList.push(...topology.configsvr.nodes);

        for (let shardName of Object.keys(topology.shards)) {
            const shard = topology.shards[shardName];

            if (shard.type === Topology.kStandalone) {
                hostList.push(shard.mongod);
            } else if (shard.type === Topology.kReplicaSet) {
                hostList.push(...shard.nodes);
            } else {
                throw new Error('Unrecognized topology format: ' + tojson(topology));
            }
        }
    } else {
        throw new Error('Unrecognized topology format: ' + tojson(topology));
    }

    new CollectionValidator().validateNodes(hostList);

})();
