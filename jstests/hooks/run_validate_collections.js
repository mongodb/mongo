// Runner for validateCollections that runs full validation on all collections when loaded into
// the merizo shell.
'use strict';

(function() {
    load('jstests/libs/discover_topology.js');      // For Topology and DiscoverTopology.
    load('jstests/hooks/validate_collections.js');  // For CollectionValidator.

    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a merizod?');
    const topology = DiscoverTopology.findConnectedNodes(db.getMerizo());

    const hostList = [];
    let setFCVHost;

    if (topology.type === Topology.kStandalone) {
        hostList.push(topology.merizod);
        setFCVHost = topology.merizod;
    } else if (topology.type === Topology.kReplicaSet) {
        hostList.push(...topology.nodes);
        setFCVHost = topology.primary;
    } else if (topology.type === Topology.kShardedCluster) {
        hostList.push(...topology.configsvr.nodes);

        for (let shardName of Object.keys(topology.shards)) {
            const shard = topology.shards[shardName];

            if (shard.type === Topology.kStandalone) {
                hostList.push(shard.merizod);
            } else if (shard.type === Topology.kReplicaSet) {
                hostList.push(...shard.nodes);
            } else {
                throw new Error('Unrecognized topology format: ' + tojson(topology));
            }
        }
        // Any of the merizos instances can be used for setting FCV.
        setFCVHost = topology.merizos.nodes[0];
    } else {
        throw new Error('Unrecognized topology format: ' + tojson(topology));
    }

    new CollectionValidator().validateNodes(hostList, setFCVHost);

})();
