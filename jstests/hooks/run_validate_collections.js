// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
load('jstests/libs/discover_topology.js');      // For Topology and DiscoverTopology.
load('jstests/hooks/validate_collections.js');  // For CollectionValidator.

assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

const hostList = [];
let setFCVHost;

if (topology.type === Topology.kStandalone) {
    hostList.push(topology.mongod);
    setFCVHost = topology.mongod;
} else if (topology.type === Topology.kReplicaSet) {
    hostList.push(...topology.nodes);
    setFCVHost = topology.primary;
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
    // Any of the mongos instances can be used for setting FCV.
    setFCVHost = topology.mongos.nodes[0];
} else {
    throw new Error('Unrecognized topology format: ' + tojson(topology));
}

new CollectionValidator().validateNodes(hostList, setFCVHost);
})();
