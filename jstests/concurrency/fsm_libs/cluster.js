'use strict';

/**
 * Represents a BongoDB cluster.
 */
load('jstests/hooks/validate_collections.js');  // Loads the validateCollections function.

var Cluster = function(options) {
    if (!(this instanceof Cluster)) {
        return new Cluster(options);
    }

    function getObjectKeys(obj) {
        var values = [];
        if (typeof obj !== "object") {
            return values;
        }
        Object.keys(obj).forEach(key => {
            if (key.indexOf('.') > -1) {
                throw new Error('illegal key specified ' + key);
            }
            var subKeys = getObjectKeys(obj[key]);
            if (subKeys.length === 0) {
                values.push(key);
            } else {
                subKeys.forEach(subKey => {
                    values.push(key + "." + subKey);
                });
            }
        });
        return values;
    }

    function validateClusterOptions(options) {
        var allowedKeys = [
            'masterSlave',
            'replication.enabled',
            'replication.numNodes',
            'sameCollection',
            'sameDB',
            'setupFunctions',
            'sharded.enabled',
            'sharded.enableAutoSplit',
            'sharded.enableBalancer',
            'sharded.numBongos',
            'sharded.numShards',
            'teardownFunctions'
        ];

        getObjectKeys(options).forEach(function(option) {
            assert.contains(option,
                            allowedKeys,
                            'invalid option: ' + tojson(option) + '; valid options are: ' +
                                tojson(allowedKeys));
        });

        options.masterSlave = options.masterSlave || false;
        assert.eq('boolean', typeof options.masterSlave);

        options.replication = options.replication || {};
        assert.eq('object', typeof options.replication);

        options.replication.enabled = options.replication.enabled || false;
        assert.eq('boolean', typeof options.replication.enabled);

        if (typeof options.replication.numNodes !== 'undefined') {
            assert(options.replication.enabled,
                   "Must have replication.enabled be true if 'replication.numNodes' is specified");
        }

        options.replication.numNodes = options.replication.numNodes || 3;
        assert.eq('number', typeof options.replication.numNodes);

        options.sameCollection = options.sameCollection || false;
        assert.eq('boolean', typeof options.sameCollection);

        options.sameDB = options.sameDB || false;
        assert.eq('boolean', typeof options.sameDB);

        options.sharded = options.sharded || {};
        assert.eq('object', typeof options.sharded);

        options.sharded.enabled = options.sharded.enabled || false;
        assert.eq('boolean', typeof options.sharded.enabled);

        if (typeof options.sharded.enableAutoSplit !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'sharded.enableAutoSplit' is specified");
        }

        options.sharded.enableAutoSplit = options.sharded.enableAutoSplit || false;
        assert.eq('boolean', typeof options.sharded.enableAutoSplit);

        if (typeof options.sharded.enableBalancer !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'sharded.enableBalancer' is specified");
        }

        options.sharded.enableBalancer = options.sharded.enableBalancer || false;
        assert.eq('boolean', typeof options.sharded.enableBalancer);

        if (typeof options.sharded.numBongos !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'sharded.numBongos' is specified");
        }

        options.sharded.numBongos = options.sharded.numBongos || 2;
        assert.eq('number', typeof options.sharded.numBongos);

        if (typeof options.sharded.numShards !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'sharded.numShards' is specified");
        }

        options.sharded.numShards = options.sharded.numShards || 2;
        assert.eq('number', typeof options.sharded.numShards);

        options.setupFunctions = options.setupFunctions || {};
        assert.eq('object', typeof options.setupFunctions);

        options.setupFunctions.bongod = options.setupFunctions.bongod || [];
        assert(Array.isArray(options.setupFunctions.bongod),
               'Expected setupFunctions.bongod to be an array');
        assert(options.setupFunctions.bongod.every(f => (typeof f === 'function')),
               'Expected setupFunctions.bongod to be an array of functions');

        if (typeof options.setupFunctions.bongos !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'setupFunctions.bongos' is specified");
        }

        options.setupFunctions.bongos = options.setupFunctions.bongos || [];
        assert(Array.isArray(options.setupFunctions.bongos),
               'Expected setupFunctions.bongos to be an array');
        assert(options.setupFunctions.bongos.every(f => (typeof f === 'function')),
               'Expected setupFunctions.bongos to be an array of functions');

        options.teardownFunctions = options.teardownFunctions || {};
        assert.eq('object', typeof options.teardownFunctions);

        options.teardownFunctions.bongod = options.teardownFunctions.bongod || [];
        assert(Array.isArray(options.teardownFunctions.bongod),
               'Expected teardownFunctions.bongod to be an array');
        assert(options.teardownFunctions.bongod.every(f => (typeof f === 'function')),
               'Expected teardownFunctions.bongod to be an array of functions');

        if (typeof options.teardownFunctions.bongos !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'teardownFunctions.bongos' is specified");
        }

        options.teardownFunctions.bongos = options.teardownFunctions.bongos || [];
        assert(Array.isArray(options.teardownFunctions.bongos),
               'Expected teardownFunctions.bongos to be an array');
        assert(options.teardownFunctions.bongos.every(f => (typeof f === 'function')),
               'Expected teardownFunctions.bongos to be an array of functions');

        assert(!options.masterSlave || !options.replication.enabled,
               "Both 'masterSlave' and " + "'replication.enabled' cannot be true");
        assert(!options.masterSlave || !options.sharded.enabled,
               "Both 'masterSlave' and 'sharded.enabled' cannot" + "be true");
    }

    function makeReplSetTestConfig(numReplSetNodes) {
        const REPL_SET_VOTING_LIMIT = 7;
        // Workaround for SERVER-26893 to specify when numReplSetNodes > REPL_SET_VOTING_LIMIT.
        var rstConfig = [];
        for (var i = 0; i < numReplSetNodes; i++) {
            rstConfig[i] = {};
            if (i >= REPL_SET_VOTING_LIMIT) {
                rstConfig[i].rsConfig = {priority: 0, votes: 0};
            }
        }
        return rstConfig;
    }

    var conn;

    var st;

    var initialized = false;
    var clusterStartTime;

    var _conns = {bongos: [], bongod: []};
    var nextConn = 0;
    var replSets = [];

    validateClusterOptions(options);
    Object.freeze(options);

    this.setup = function setup() {
        var verbosityLevel = 0;
        const REPL_SET_INITIATE_TIMEOUT_MS = 5 * 60 * 1000;

        if (initialized) {
            throw new Error('cluster has already been initialized');
        }

        if (options.sharded.enabled) {
            // TODO: allow 'options' to specify the number of shards and bongos processes
            var shardConfig = {
                shards: options.sharded.numShards,
                bongos: options.sharded.numBongos,
                verbose: verbosityLevel,
                other: {
                    enableAutoSplit: options.sharded.enableAutoSplit,
                    enableBalancer: options.sharded.enableBalancer,
                }
            };

            // TODO: allow 'options' to specify an 'rs' config
            if (options.replication.enabled) {
                shardConfig.rs = {
                    nodes: makeReplSetTestConfig(options.replication.numNodes),
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

            conn = st.s;  // bongos

            this.teardown = function teardown() {
                options.teardownFunctions.bongod.forEach(this.executeOnBongodNodes);
                options.teardownFunctions.bongos.forEach(this.executeOnBongosNodes);

                st.stop();
            };

            // Save all bongos and bongod connections
            var i = 0;
            var bongos = st.s0;
            var bongod = st.d0;
            while (bongos) {
                _conns.bongos.push(bongos);
                ++i;
                bongos = st['s' + i];
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
            while (bongod) {
                _conns.bongod.push(bongod);
                ++i;
                bongod = st['d' + i];
            }
        } else if (options.replication.enabled) {
            var replSetConfig = {
                nodes: makeReplSetTestConfig(options.replication.numNodes),
                // Increase the oplog size (in MB) to prevent rollover during write-heavy workloads
                oplogSize: 1024,
                nodeOptions: {verbose: verbosityLevel}
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
                options.teardownFunctions.bongod.forEach(this.executeOnBongodNodes);

                rst.stopSet();
            };

            this._addReplicaSetConns(rst);

        } else if (options.masterSlave) {
            var rt = new ReplTest('replTest');

            var master = rt.start(true);
            var slave = rt.start(false);
            conn = master;

            master.adminCommand({setParameter: 1, logLevel: verbosityLevel});
            slave.adminCommand({setParameter: 1, logLevel: verbosityLevel});

            this.teardown = function teardown() {
                options.teardownFunctions.bongod.forEach(this.executeOnBongodNodes);

                rt.stop();
            };

            _conns.bongod = [master, slave];

        } else {  // standalone server
            conn = db.getBongo();
            db.adminCommand({setParameter: 1, logLevel: verbosityLevel});

            _conns.bongod = [conn];
        }

        initialized = true;
        clusterStartTime = new Date();

        options.setupFunctions.bongod.forEach(this.executeOnBongodNodes);
        if (options.sharded) {
            options.setupFunctions.bongos.forEach(this.executeOnBongosNodes);
        }
    };

    this._addReplicaSetConns = function _addReplicaSetConns(rsTest) {
        _conns.bongod.push(rsTest.getPrimary());
        rsTest.getSecondaries().forEach(function(secondaryConn) {
            _conns.bongod.push(secondaryConn);
        });
    };

    this.executeOnBongodNodes = function executeOnBongodNodes(fn) {
        assert(initialized, 'cluster must be initialized first');

        if (!fn || typeof(fn) !== 'function' || fn.length !== 1) {
            throw new Error('bongod function must be a function that takes a db as an argument');
        }
        _conns.bongod.forEach(function(bongodConn) {
            fn(bongodConn.getDB('admin'));
        });
    };

    this.executeOnBongosNodes = function executeOnBongosNodes(fn) {
        assert(initialized, 'cluster must be initialized first');

        if (!fn || typeof(fn) !== 'function' || fn.length !== 1) {
            throw new Error('bongos function must be a function that takes a db as an argument');
        }
        _conns.bongos.forEach(function(bongosConn) {
            fn(bongosConn.getDB('admin'));
        });
    };

    this.teardown = function teardown() {
        assert(initialized, 'cluster must be initialized first');
        options.teardownFunctions.bongod.forEach(this.executeOnBongodNodes);
    };

    this.getDB = function getDB(dbName) {
        assert(initialized, 'cluster must be initialized first');
        return conn.getDB(dbName);
    };

    this.getHost = function getHost() {
        assert(initialized, 'cluster must be initialized first');

        // Alternate bongos connections for sharded clusters
        if (this.isSharded()) {
            return _conns.bongos[nextConn++ % _conns.bongos.length].host;
        }
        return conn.host;
    };

    this.isSharded = function isSharded() {
        return options.sharded.enabled;
    };

    this.isReplication = function isReplication() {
        return options.replication.enabled;
    };

    this.shardCollection = function shardCollection() {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');
        st.shardColl.apply(st, arguments);
    };

    // Provide a serializable form of the cluster for use in workload states. This
    // method is required because we don't currently support the serialization of Bongo
    // connection objects.
    //
    // Serialized format:
    // {
    //      bongos: [
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

        var cluster = {bongos: [], config: [], shards: {}};

        var i = 0;
        var bongos = st.s0;
        while (bongos) {
            cluster.bongos.push(bongos.name);
            ++i;
            bongos = st['s' + i];
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
                // If the shard is a standalone bongod, the format of st.shard0.name in ShardingTest
                // is "localhost:20006".
                cluster.shards[shard.shardName] = [shard.name];
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
        return this.isSharded() && options.sharded.enableBalancer;
    };

    this.isAutoSplitEnabled = function isAutoSplitEnabled() {
        return this.isSharded() && options.sharded.enableAutoSplit;
    };

    this.validateAllCollections = function validateAllCollections(phase) {
        assert(initialized, 'cluster must be initialized first');

        var _validateCollections = function _validateCollections(db) {
            // Validate all the collections on each node.
            var res = db.adminCommand({listDatabases: 1});
            assert.commandWorked(res);
            res.databases.forEach(dbInfo => {
                if (!validateCollections(db.getSiblingDB(dbInfo.name), {full: true})) {
                    throw new Error(phase + ' collection validation failed');
                }
            });
        };

        var startTime = Date.now();
        jsTest.log('Starting to validate collections ' + phase);
        this.executeOnBongodNodes(_validateCollections);
        this.executeOnBongosNodes(_validateCollections);
        var totalTime = Date.now() - startTime;
        jsTest.log('Finished validating collections in ' + totalTime + ' ms, ' + phase);
    };

    this.checkReplicationConsistency = function checkReplicationConsistency(dbBlacklist, phase) {
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

            if (shouldCheckDBHashes) {
                jsTest.log('Starting consistency checks for replica set with ' + primary.host +
                           ' assumed to still be primary, ' + phase);

                // Compare the dbhashes of the primary and secondaries.
                rst.checkReplicatedDataHashes(phase, dbBlacklist);
                var totalTime = Date.now() - startTime;
                jsTest.log('Finished consistency checks of replica set with ' + primary.host +
                           ' as primary in ' + totalTime + ' ms, ' + phase);
            } else {
                jsTest.log('Skipping consistency checks when the balancer is enabled, ' +
                           'for replica set with ' + primary.host +
                           ' assumed to still be primary, ' + phase);

                // Get the latest optime from the primary.
                var replSetStatus = primary.adminCommand({replSetGetStatus: 1});
                assert.commandWorked(replSetStatus, phase + ', error getting replication status');

                var primaryInfo = replSetStatus.members.find(memberInfo => memberInfo.self);
                assert(primaryInfo !== undefined,
                       phase + ', failed to find self in replication status: ' +
                           tojson(replSetStatus));

                // Wait for all previous workload operations to complete, with "getLastError".
                res = primary.getDB('test').runCommand({
                    getLastError: 1,
                    w: options.replication.numNodes,
                    wtimeout: 5 * 60 * 1000,
                    wOpTime: primaryInfo.optime
                });
                assert.commandWorked(res, phase + ', error awaiting replication');
            }

        });
    };

    this.recordConfigServerData = function recordConfigServerData(configServer) {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');

        var data = {};
        var configDB = configServer.getDB('config');

        // We record the contents of the 'lockpings' and 'locks' collections to make it easier to
        // debug issues with distributed locks in the sharded cluster.
        data.lockpings = configDB.lockpings.find({ping: {$gte: clusterStartTime}}).toArray();

        // We suppress some fields from the result set to reduce the amount of data recorded.
        data.locks =
            configDB.locks.find({when: {$gte: clusterStartTime}}, {process: 0, ts: 0}).toArray();

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

    this.isRunningWiredTigerLSM = function isRunningWiredTigerLSM() {
        var adminDB = this.getDB('admin');

        if (this.isSharded()) {
            // Get the storage engine the sharded cluster is configured to use from one of the
            // shards since bongos won't report it.
            adminDB = st.shard0.getDB('admin');
        }

        var res = adminDB.runCommand({getCmdLineOpts: 1});
        assert.commandWorked(res, 'failed to get command line options');

        var wiredTigerOptions = res.parsed.storage.wiredTiger || {};
        var wiredTigerCollectionConfig = wiredTigerOptions.collectionConfig || {};
        var wiredTigerConfigString = wiredTigerCollectionConfig.configString || '';

        return wiredTigerConfigString === 'type=lsm';
    };
};

/**
 * Returns true if 'clusterOptions' represents a standalone bongod,
 * and false otherwise.
 */
Cluster.isStandalone = function isStandalone(clusterOptions) {
    return !clusterOptions.sharded.enabled && !clusterOptions.replication.enabled &&
        !clusterOptions.masterSlave;
};
