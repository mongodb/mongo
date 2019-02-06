'use strict';

// The tojson() function that is commonly used to build up assertion messages doesn't support the
// Symbol type, so we just use unique string values instead.
var Topology = {
    kStandalone: 'stand-alone',
    kRouter: 'mongos router',
    kReplicaSet: 'replica set',
    kShardedCluster: 'sharded cluster',
};

var DiscoverTopology = (function() {
    const kDefaultConnectFn = (host) => new Mongo(host);

    function getDataMemberConnectionStrings(conn) {
        const res = conn.adminCommand({isMaster: 1});

        if (!res.hasOwnProperty('setName')) {
            // 'conn' represents a connection to a stand-alone mongod.
            return {type: Topology.kStandalone, mongod: conn.host};
        }

        // The "passives" field contains the list of unelectable (priority=0) secondaries
        // and is omitted from the server's response when there are none.
        res.passives = res.passives || [];
        return {
            type: Topology.kReplicaSet,
            primary: res.primary,
            nodes: [...res.hosts, ...res.passives]
        };
    }

    function findConnectedNodesViaMongos(conn, options) {
        function getConfigServerConnectionString() {
            const shardMap = conn.adminCommand({getShardMap: 1});

            if (!shardMap.hasOwnProperty('map')) {
                throw new Error(
                    'Expected "getShardMap" command to return an object with a "map" field: ' +
                    tojson(shardMap));
            }

            if (!shardMap.map.hasOwnProperty('config')) {
                throw new Error(
                    'Expected "getShardMap" command to return an object with a "map.config"' +
                    ' field: ' + tojson(shardMap));
            }

            return shardMap.map.config;
        }

        const connectFn =
            options.hasOwnProperty('connectFn') ? options.connectFn : kDefaultConnectFn;

        const configsvrConn = connectFn(getConfigServerConnectionString());
        const configsvrHosts = getDataMemberConnectionStrings(configsvrConn);

        const shards = assert.commandWorked(conn.adminCommand({listShards: 1})).shards;
        const shardHosts = {};

        for (let shardInfo of shards) {
            const shardConn = connectFn(shardInfo.host);
            shardHosts[shardInfo._id] = getDataMemberConnectionStrings(shardConn);
        }

        // Discover mongos URIs from the connection string. If a mongos is not passed in explicitly,
        // it will not be discovered.
        const mongosUris = new MongoURI("mongodb://" + conn.host);

        const mongos = {
            type: Topology.kRouter,
            nodes: mongosUris.servers.map(uriObj => uriObj.server),
        };

        return {
            type: Topology.kShardedCluster,
            configsvr: configsvrHosts,
            shards: shardHosts,
            mongos: mongos,
        };
    }

    /**
     * Returns an object describing the topology of the mongod processes reachable from 'conn'.
     * The "connectFn" property can be optionally specified to support custom retry logic when
     * making connection attempts without overriding the Mongo constructor itself.
     *
     * For a stand-alone mongod, an object of the form
     *   {type: Topology.kStandalone, mongod: <conn-string>}
     * is returned.
     *
     * For a replica set, an object of the form
     *   {
     *     type: Topology.kReplicaSet,
     *     primary: <primary-conn-string>,
     *     nodes: [<conn-string1>, <conn-string2>, ...],
     *   }
     * is returned.
     *
     * For a sharded cluster, an object of the form
     *   {
     *     type: Topology.kShardedCluster,
     *     configsvr: {nodes: [...]},
     *     shards: {
     *       <shard-name1>: {type: Topology.kStandalone, mongod: ...},
     *       <shard-name2>: {type: Topology.kReplicaSet,
     *                       primary: <primary-conn-string>,
     *                       nodes: [...]},
     *       ...
     *     },
     *     mongos: {
     *       type: Topology.kRouter,
     *       nodes: [...],
     *     }
     *   }
     * is returned, where the description for each shard depends on whether it is a stand-alone
     * shard or a replica set shard.
     */
    function findConnectedNodes(conn, options = {connectFn: kDefaultConnectFn}) {
        const isMongod = conn.adminCommand({isMaster: 1}).msg !== 'isdbgrid';

        if (isMongod) {
            return getDataMemberConnectionStrings(conn);
        }

        return findConnectedNodesViaMongos(conn, options);
    }

    function addNonConfigNodesToList(topology, hostList) {
        if (topology.type === Topology.kStandalone) {
            hostList.push(topology.mongod);
        } else if (topology.type === Topology.kReplicaSet) {
            hostList.push(...topology.nodes);
        } else if (topology.type === Topology.kShardedCluster) {
            for (let shardName of Object.keys(topology.shards)) {
                const shardTopology = topology.shards[shardName];
                addNonConfigNodesToList(shardTopology, hostList);
            }
            hostList.push(...topology.mongos.nodes);
        } else {
            throw new Error('Unrecognized topology format: ' + tojson(topology));
        }
    }

    /**
     * Return list of nodes in the cluster given a connection NOT including config servers (if
     * there are any).
     */
    function findNonConfigNodes(conn) {
        const topology = findConnectedNodes(conn);
        let hostList = [];
        addNonConfigNodesToList(topology, hostList);
        return hostList;
    }

    return {findConnectedNodes: findConnectedNodes, findNonConfigNodes: findNonConfigNodes};
})();
