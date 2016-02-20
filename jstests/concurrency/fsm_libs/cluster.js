'use strict';

/**
 * Represents a MongoDB cluster.
 */

var Cluster = function(options) {
    if (!(this instanceof Cluster)) {
        return new Cluster(options);
    }

    function validateClusterOptions(options) {
        var allowedKeys = [
            'enableBalancer',
            'masterSlave',
            'replication',
            'sameCollection',
            'sameDB',
            'setupFunctions',
            'sharded',
            'teardownFunctions',
            'useLegacyConfigServers'
        ];

        Object.keys(options).forEach(function(option) {
            assert.contains(option, allowedKeys,
                'invalid option: ' + tojson(option) +
                '; valid options are: ' + tojson(allowedKeys));
        });

        options.enableBalancer = options.enableBalancer || false;
        assert.eq('boolean', typeof options.enableBalancer);

        options.masterSlave = options.masterSlave || false;
        assert.eq('boolean', typeof options.masterSlave);

        options.replication = options.replication || false;
        assert.eq('boolean', typeof options.replication);

        options.sameCollection = options.sameCollection || false;
        assert.eq('boolean', typeof options.sameCollection);

        options.sameDB = options.sameDB || false;
        assert.eq('boolean', typeof options.sameDB);

        options.sharded = options.sharded || false;
        assert.eq('boolean', typeof options.sharded);

        if (typeof options.useLegacyConfigServers !== 'undefined') {
            assert(options.sharded, "Must be sharded if 'useLegacyConfigServers' is specified");
        }

        options.useLegacyConfigServers = options.useLegacyConfigServers || false;
        assert.eq('boolean', typeof options.useLegacyConfigServers);

        options.setupFunctions = options.setupFunctions || {};
        assert.eq('object', typeof options.setupFunctions);

        options.setupFunctions.mongod = options.setupFunctions.mongod || [];
        assert(Array.isArray(options.setupFunctions.mongod),
               'Expected setupFunctions.mongod to be an array');
        assert(options.setupFunctions.mongod.every(f => (typeof f === 'function')),
               'Expected setupFunctions.mongod to be an array of functions');

        if (typeof options.setupFunctions.mongos !== 'undefined') {
            assert(options.sharded, "Must be sharded if 'setupFunctions.mongos' is specified");
        }

        options.setupFunctions.mongos = options.setupFunctions.mongos || [];
        assert(Array.isArray(options.setupFunctions.mongos),
               'Expected setupFunctions.mongos to be an array');
        assert(options.setupFunctions.mongos.every(f => (typeof f === 'function')),
               'Expected setupFunctions.mongos to be an array of functions');

        options.teardownFunctions = options.teardownFunctions || {};
        assert.eq('object', typeof options.teardownFunctions);

        options.teardownFunctions.mongod = options.teardownFunctions.mongod || [];
        assert(Array.isArray(options.teardownFunctions.mongod),
               'Expected teardownFunctions.mongod to be an array');
        assert(options.teardownFunctions.mongod.every(f => (typeof f === 'function')),
               'Expected teardownFunctions.mongod to be an array of functions');

        if (typeof options.teardownFunctions.mongos !== 'undefined') {
            assert(options.sharded, "Must be sharded if 'teardownFunctions.mongos' is specified");
        }

        options.teardownFunctions.mongos = options.teardownFunctions.mongos || [];
        assert(Array.isArray(options.teardownFunctions.mongos),
               'Expected teardownFunctions.mongos to be an array');
        assert(options.teardownFunctions.mongos.every(f => (typeof f === 'function')),
               'Expected teardownFunctions.mongos to be an array of functions');

        assert(!options.masterSlave || !options.replication, "Both 'masterSlave' and " +
               "'replication' cannot be true");
        assert(!options.masterSlave || !options.sharded, "Both 'masterSlave' and 'sharded' cannot" +
               "be true");
    }

    var conn;

    var st;

    var initialized = false;
    var clusterStartTime;

    var _conns = {
        mongos: [],
        mongod: []
    };
    var nextConn = 0;
    var replSets = [];

    // TODO: Define size of replica set from options
    var replSetNodes = 3;

    validateClusterOptions(options);
    Object.freeze(options);

    this.setup = function setup() {
        var verbosityLevel = 0;
        const REPL_SET_INITIATE_TIMEOUT_MS = 5 * 60 * 1000;

        if (initialized) {
            throw new Error('cluster has already been initialized');
        }

        if (options.sharded) {
            // TODO: allow 'options' to specify the number of shards and mongos processes
            var shardConfig = {
                shards: 2,
                mongos: 2,
                // Legacy config servers are pre-3.2 style, 3-node non-replica-set config servers
                sync: options.useLegacyConfigServers,
                verbose: verbosityLevel,
                other: { enableBalancer: options.enableBalancer }
            };

            // TODO: allow 'options' to specify an 'rs' config
            if (options.replication) {
                shardConfig.rs = {
                    nodes: replSetNodes,
                    // Increase the oplog size (in MB) to prevent rollover
                    // during write-heavy workloads
                    oplogSize: 1024,
                    verbose: verbosityLevel
                };
                shardConfig.rsOptions = {
                    // Specify a longer timeout for replSetInitiate, to ensure that
                    // slow hardware has sufficient time for file pre-allocation.
                    initiateTimeout: REPL_SET_INITIATE_TIMEOUT_MS,
                };
            }

            st = new ShardingTest(shardConfig);

            conn = st.s; // mongos

            this.teardown = function teardown() {
                options.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);
                options.teardownFunctions.mongos.forEach(this.executeOnMongosNodes);

                st.stop();
            };

            // Save all mongos and mongod connections
            var i = 0;
            var mongos = st.s0;
            var mongod = st.d0;
            while (mongos) {
                _conns.mongos.push(mongos);
                ++i;
                mongos = st['s' + i];
            }
            if (options.replication) {
                var rsTest = st.rs0;

                i = 0;
                while (rsTest) {
                    this._addReplicaSetConns(rsTest);
                    replSets.push(rsTest);
                    ++i;
                    rsTest = st['rs' + i];
                }
            }
            i = 0;
            while (mongod) {
                _conns.mongod.push(mongod);
                ++i;
                mongod = st['d' + i];
            }
        } else if (options.replication) {
            // TODO: allow 'options' to specify the number of nodes
            var replSetConfig = {
                nodes: replSetNodes,
                // Increase the oplog size (in MB) to prevent rollover during write-heavy workloads
                oplogSize: 1024,
                nodeOptions: { verbose: verbosityLevel }
            };

            var rst = new ReplSetTest(replSetConfig);
            rst.startSet();

            // Send the replSetInitiate command and wait for initialization, with an increased
            // timeout. This should provide sufficient time for slow hardware, where files may need
            // to be pre-allocated.
            rst.initiate(null, null, REPL_SET_INITIATE_TIMEOUT_MS);
            rst.awaitSecondaryNodes();

            conn = rst.getPrimary();
            replSets = [rst];

            this.teardown = function teardown() {
                options.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);

                rst.stopSet();
            };

            this._addReplicaSetConns(rst);

        } else if (options.masterSlave) {
            var rt = new ReplTest('replTest');

            var master = rt.start(true);
            var slave = rt.start(false);
            conn = master;

            master.adminCommand({ setParameter: 1, logLevel: verbosityLevel });
            slave.adminCommand({ setParameter: 1, logLevel: verbosityLevel });

            this.teardown = function teardown() {
                options.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);

                rt.stop();
            };

            _conns.mongod = [master, slave];

        } else { // standalone server
            conn = db.getMongo();
            db.adminCommand({ setParameter: 1, logLevel: verbosityLevel });

            _conns.mongod = [conn];
        }

        initialized = true;
        clusterStartTime = new Date();

        options.setupFunctions.mongod.forEach(this.executeOnMongodNodes);
        if (options.sharded) {
            options.setupFunctions.mongos.forEach(this.executeOnMongosNodes);
        }
    };


    this._addReplicaSetConns = function _addReplicaSetConns(rsTest) {
        _conns.mongod.push(rsTest.getPrimary());
        rsTest.getSecondaries().forEach(function (secondaryConn) {
            _conns.mongod.push(secondaryConn);
        });
    };

    this.executeOnMongodNodes = function executeOnMongodNodes(fn) {
        assert(initialized, 'cluster must be initialized first');

        if (!fn || typeof(fn) !== 'function' || fn.length !== 1) {
            throw new Error('mongod function must be a function that takes a db as an argument');
        }
        _conns.mongod.forEach(function(mongodConn) {
            fn(mongodConn.getDB('admin'));
        });
    };

    this.executeOnMongosNodes = function executeOnMongosNodes(fn) {
        assert(initialized, 'cluster must be initialized first');

        if (!fn || typeof(fn) !== 'function' || fn.length !== 1) {
            throw new Error('mongos function must be a function that takes a db as an argument');
        }
        _conns.mongos.forEach(function(mongosConn) {
            fn(mongosConn.getDB('admin'));
        });
    };

    this.teardown = function teardown() {
        assert(initialized, 'cluster must be initialized first');
        options.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);
    };

    this.getDB = function getDB(dbName) {
        assert(initialized, 'cluster must be initialized first');
        return conn.getDB(dbName);
    };

    this.getHost = function getHost() {
        assert(initialized, 'cluster must be initialized first');

        // Alternate mongos connections for sharded clusters
        if (this.isSharded()) {
            return _conns.mongos[nextConn++ % _conns.mongos.length].host;
        }
        return conn.host;
    };

    this.isSharded = function isSharded() {
        return options.sharded;
    };

    this.isReplication = function isReplication() {
        return options.replication;
    };

    this.isUsingLegacyConfigServers = function isUsingLegacyConfigServers() {
        assert(this.isSharded(), 'cluster is not sharded');
        return options.useLegacyConfigServers;
    };

    this.shardCollection = function shardCollection() {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');
        st.shardColl.apply(st, arguments);
    };

    // Provide a serializable form of the cluster for use in workload states. This
    // method is required because we don't currently support the serialization of Mongo
    // connection objects.
    //
    // Serialized format:
    // {
    //      mongos: [
    //          "localhost:30998",
    //          "localhost:30999"
    //      ],
    //      config: [
    //          "localhost:29000",
    //          "localhost:29001",
    //          "localhost:29002"
    //      ],
    //      shards: {
    //          "test-rs0": [
    //              "localhost:20006",
    //              "localhost:20007",
    //              "localhost:20008"
    //          ],
    //          "test-rs1": [
    //              "localhost:20009",
    //              "localhost:20010",
    //              "localhost:20011"
    //          ]
    //      }
    // }
    this.getSerializedCluster = function getSerializedCluster() {
        assert(initialized, 'cluster must be initialized first');

        // TODO: Add support for non-sharded clusters.
        if (!this.isSharded()) {
            return '';
        }

        var cluster = {
            mongos: [],
            config: [],
            shards: {}
        };

        var i = 0;
        var mongos = st.s0;
        while (mongos) {
            cluster.mongos.push(mongos.name);
            ++i;
            mongos = st['s' + i];
        }

        i = 0;
        var config = st.c0;
        while (config) {
            cluster.config.push(config.name);
            ++i;
            config = st['c' + i];
        }

        i = 0;
        var shard = st.shard0;
        while (shard) {
            if (shard.name.includes('/')) {
                // If the shard is a replica set, the format of st.shard0.name in ShardingTest is
                // "test-rs0/localhost:20006,localhost:20007,localhost:20008".
                var [setName, shards] = shard.name.split('/');
                cluster.shards[setName] = shards.split(',');
            } else {
                // If the shard is a standalone mongod, the format of st.shard0.name in ShardingTest
                // is "localhost:20006".
                cluster.shards[i] = [shard.name];
            }
            ++i;
            shard = st['shard' + i];
        }
        return cluster;
    };

    this.startBalancer = function startBalancer() {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');
        st.startBalancer();
    };

    this.stopBalancer = function stopBalancer() {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');
        st.stopBalancer();
    };

    this.isBalancerEnabled = function isBalancerEnabled() {
        return this.isSharded() && options.enableBalancer;
    };

    this.checkDBHashes = function checkDBHashes(rst, dbBlacklist, phase) {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isReplication(), 'cluster is not a replica set');

        // Use liveNodes.master instead of getPrimary() to avoid the detection of a new primary.
        var primary = rst.liveNodes.master;

        var res = primary.adminCommand({ listDatabases: 1 });
        assert.commandWorked(res);

        res.databases.forEach(dbInfo => {
            var dbName = dbInfo.name;
            if (Array.contains(dbBlacklist, dbName)) {
                return;
            }

            var dbHashes = rst.getHashes(dbName);
            var primaryDBHash = dbHashes.master;
            assert.commandWorked(primaryDBHash);

            dbHashes.slaves.forEach(secondaryDBHash => {
                assert.commandWorked(secondaryDBHash);

                var primaryNumCollections = Object.keys(primaryDBHash.collections).length;
                var secondaryNumCollections = Object.keys(secondaryDBHash.collections).length;

                assert.eq(primaryNumCollections, secondaryNumCollections,
                          phase + ', the primary and secondary have a different number of' +
                          ' collections: ' + tojson(dbHashes));

                // Only compare the dbhashes of non-capped collections because capped collections
                // are not necessarily truncated at the same points across replica set members.
                var collNames = Object.keys(primaryDBHash.collections).filter(collName =>
                    !primary.getDB(dbName)[collName].isCapped());

                collNames.forEach(collName => {
                    assert.eq(primaryDBHash.collections[collName],
                              secondaryDBHash.collections[collName],
                              phase + ', the primary and secondary have a different hash for the' +
                              ' collection ' + dbName + '.' + collName + ': ' + tojson(dbHashes));
                });

                if (collNames.length === primaryNumCollections) {
                    // If the primary and secondary have the same hashes for all the collections on
                    // the database and there aren't any capped collections, then the hashes for the
                    // whole database should match.
                    assert.eq(primaryDBHash.md5,
                              secondaryDBHash.md5,
                              phase + ', the primary and secondary have a different hash for the ' +
                              dbName + ' database: ' + tojson(dbHashes));
                }
            });
        });
    };

    this.checkReplicationConsistency = function checkReplicationConsistency(dbBlacklist,
                                                                            phase,
                                                                            ttlIndexExists) {
        assert(initialized, 'cluster must be initialized first');

        if (!this.isReplication()) {
            return;
        }

        var shouldCheckDBHashes = !this.isBalancerEnabled();

        replSets.forEach(rst => {
            var startTime = Date.now();
            var res;

            // Use liveNodes.master instead of getPrimary() to avoid the detection of a new primary.
            var primary = rst.liveNodes.master;
            jsTest.log('Starting consistency checks for replica set with ' + primary.host +
                       ' assumed to still be primary, ' + phase);

            if (shouldCheckDBHashes && ttlIndexExists) {
                // Lock the primary to prevent the TTL monitor from deleting expired documents in
                // the background while we are getting the dbhashes of the replica set members.
                assert.commandWorked(primary.adminCommand({ fsync: 1, lock: 1 }),
                                     phase + ', failed to lock the primary');
            }

            var activeException = false;
            var msg;

            try {
                // Get the latest optime from the primary.
                var replSetStatus = primary.adminCommand({ replSetGetStatus: 1 });
                assert.commandWorked(replSetStatus,
                                     phase + ', error getting replication status');

                var primaryInfo = replSetStatus.members.find(memberInfo => memberInfo.self);
                assert(primaryInfo !== undefined,
                       phase + ', failed to find self in replication status: ' +
                       tojson(replSetStatus));

                // Wait for all previous workload operations to complete. We use the "getLastError"
                // command rather than a replicated write because the primary is currently
                // fsyncLock()ed to prevent the TTL monitor from running.
                res = primary.getDB('test').runCommand({
                    getLastError: 1,
                    w: replSetNodes,
                    wtimeout: 5 * 60 * 1000,
                    wOpTime: primaryInfo.optime
                });
                assert.commandWorked(res, phase + ', error awaiting replication');

                if (shouldCheckDBHashes) {
                    // Compare the dbhashes of the primary and secondaries.
                    this.checkDBHashes(rst, dbBlacklist);
                }
            } catch (e) {
                activeException = true;
                throw e;
            } finally {
                if (shouldCheckDBHashes && ttlIndexExists) {
                    // Allow writes on the primary.
                    res = primary.adminCommand({ fsyncUnlock: 1 });

                    // Returning early would suppress the exception rethrown in the catch block.
                    if (!res.ok) {
                        msg = phase + ', failed to unlock the primary, which may cause this' +
                              ' test to hang: ' + tojson(res);
                        if (activeException) {
                            jsTest.log(msg);
                        } else {
                            throw new Error(msg);
                        }
                    }
                }
            }

            var totalTime = Date.now() - startTime;
            jsTest.log('Finished consistency checks of replica set with ' + primary.host +
                       ' as primary in ' + totalTime +  ' ms, ' + phase);
        });
    };

    this.recordConfigServerData = function recordConfigServerData(configServer) {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');

        var data = {};
        var configDB = configServer.getDB('config');

        // We record the contents of the 'lockpings' and 'locks' collections to make it easier to
        // debug issues with distributed locks in the sharded cluster.
        data.lockpings = configDB.lockpings.find({ ping: { $gte: clusterStartTime } }).toArray();

        // We suppress some fields from the result set to reduce the amount of data recorded.
        data.locks = configDB.locks.find({ when: { $gte: clusterStartTime } },
                                         { process: 0, ts: 0 }).toArray();

        return data;
    };

    this.recordAllConfigServerData = function recordAllConfigServerData() {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');

        var data = {};
        st._configServers.forEach(config =>
            (data[config.host] = this.recordConfigServerData(config)));

        return data;
    };
};

/**
 * Returns true if 'clusterOptions' represents a standalone mongod,
 * and false otherwise.
 */
Cluster.isStandalone = function isStandalone(clusterOptions) {
    return !clusterOptions.sharded && !clusterOptions.replication && !clusterOptions.masterSlave;
};
