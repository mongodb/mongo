load('jstests/libs/discover_topology.js');

var FSMShardingTest = class {
    constructor(connStr) {
        /**
         * `topology` has the following format:

         {
            "type" : "sharded cluster",
            "configsvr" : {
                "type" : "replica set",
                "nodes" : [
                    "robert-macbook-pro.local:20001",
                    "robert-macbook-pro.local:20002",
                    "robert-macbook-pro.local:20003"
                ]
            },
            "shards" : {
                "__unknown_name__-rs0" : {
                    "type" : "replica set",
                    "nodes" : [
                        "robert-macbook-pro.local:20000"
                    ]
                }
            },
            "mongos" : {
                "type" : "mongos router",
                "nodes" : [
                    "robert-macbook-pro.local:20004"
                ]
            }
         }
         */

        const conn = new Mongo(connStr);

        const topology = DiscoverTopology.findConnectedNodes(conn);
        assert.eq(topology.type, Topology.kShardedCluster, 'Topology must be a sharded cluster');

        this._mongoses = [];
        for (let connStr of topology.mongos.nodes) {
            this._mongoses.push(new Mongo(connStr));
        }
        for (let mongos of this._mongoses) {
            mongos.name = mongos.host;
        }

        this._configsvr = new ReplSetTest(topology.configsvr.nodes[0]);
        for (let node of this._configsvr.nodes) {
            node.name = node.host;
        }

        // connections to replsets or mongods.
        this._shard_connections = [];
        // ReplSetTest objects for replsets shards.
        this._shard_rsts = [];
        for (let shardName of Object.keys(topology.shards)) {
            let shardTopology = topology.shards[shardName];
            let shard;
            if (shardTopology.type === Topology.kReplicaSet) {
                const shard_rst = new ReplSetTest(shardTopology.nodes[0]);
                this._shard_rsts.push(shard_rst);

                shard = new Mongo(shard_rst.getURL());
                shard.name = shard_rst.getURL();
            } else {
                shard = new Mongo(shardTopology.mongod);
                shard.name = shard.host;
            }
            shard.shardName = shardName;
            this._shard_connections.push(shard);
        }
    }

    /*
     * Setters and Getters.
     */

    s(n = 0) {
        return this._mongoses[n];
    }

    d(n = 0) {
        // Only return for non-replset shards.
        if (this._shard_rsts[n] === undefined) {
            return this._shard_connections[n];
        }
        return undefined;
    }

    /**
     * This function returns the ReplSetTest instance for a shard, whereas shard() returns
     * the connection to the shard primary.
     */
    rs(n = 0) {
        return this._shard_rsts[n];
    }

    c(n = 0) {
        return this._configsvr.nodes[n];
    }

    shard(n = 0) {
        return this._shard_connections[n];
    }

    get _configServers() {
        return this._configsvr.nodes;
    }

    /*
     * Public Functions.
     */

    shardColl(coll, shardKey) {
        assert.commandWorked(this.s(0).adminCommand({
            enableSharding: coll.getDB().toString(),
        }));

        assert.commandWorked(this.s(0).adminCommand({
            shardCollection: coll.toString(),
            key: shardKey,
        }));
    }

    /*
     * Internal Functions.
     */
};
