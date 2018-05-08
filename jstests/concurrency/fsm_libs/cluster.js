'use strict';

/**
 * Represents a MongoDB cluster.
 */
load('jstests/hooks/validate_collections.js');          // For validateCollections.
load('jstests/concurrency/fsm_libs/shard_fixture.js');  // For FSMShardingTest.

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
            'replication.enabled',
            'replication.numNodes',
            'sameCollection',
            'sameDB',
            'setupFunctions',
            'sharded.enabled',
            'sharded.enableAutoSplit',
            'sharded.enableBalancer',
            'sharded.numMongos',
            'sharded.numShards',
            'sharded.stepdownOptions',
            'sharded.stepdownOptions.configStepdown',
            'sharded.stepdownOptions.shardStepdown',
            'teardownFunctions',
            'useExistingConnectionAsSeed',
        ];

        getObjectKeys(options).forEach(function(option) {
            assert.contains(option,
                            allowedKeys,
                            'invalid option: ' + tojson(option) + '; valid options are: ' +
                                tojson(allowedKeys));
        });

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

        if (typeof options.sharded.numMongos !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'sharded.numMongos' is specified");
        }

        options.sharded.numMongos = options.sharded.numMongos || 2;
        assert.eq('number', typeof options.sharded.numMongos);

        if (typeof options.sharded.numShards !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'sharded.numShards' is specified");
        }

        options.sharded.numShards = options.sharded.numShards || 2;
        assert.eq('number', typeof options.sharded.numShards);

        options.setupFunctions = options.setupFunctions || {};
        assert.eq('object', typeof options.setupFunctions);

        options.setupFunctions.mongod = options.setupFunctions.mongod || [];
        assert(Array.isArray(options.setupFunctions.mongod),
               'Expected setupFunctions.mongod to be an array');
        assert(options.setupFunctions.mongod.every(f => (typeof f === 'function')),
               'Expected setupFunctions.mongod to be an array of functions');

        if (typeof options.setupFunctions.mongos !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'setupFunctions.mongos' is specified");
        }

        options.setupFunctions.mongos = options.setupFunctions.mongos || [];
        assert(Array.isArray(options.setupFunctions.mongos),
               'Expected setupFunctions.mongos to be an array');
        assert(options.setupFunctions.mongos.every(f => (typeof f === 'function')),
               'Expected setupFunctions.mongos to be an array of functions');

        options.setupFunctions.config = options.setupFunctions.config || [];
        assert(Array.isArray(options.setupFunctions.config),
               'Expected setupFunctions.config to be an array');
        assert(options.setupFunctions.config.every(f => (typeof f === 'function')),
               'Expected setupFunctions.config to be an array of functions');

        options.teardownFunctions = options.teardownFunctions || {};
        assert.eq('object', typeof options.teardownFunctions);

        options.teardownFunctions.mongod = options.teardownFunctions.mongod || [];
        assert(Array.isArray(options.teardownFunctions.mongod),
               'Expected teardownFunctions.mongod to be an array');
        assert(options.teardownFunctions.mongod.every(f => (typeof f === 'function')),
               'Expected teardownFunctions.mongod to be an array of functions');

        if (typeof options.teardownFunctions.mongos !== 'undefined') {
            assert(options.sharded.enabled,
                   "Must have sharded.enabled be true if 'teardownFunctions.mongos' is specified");
        }

        options.teardownFunctions.mongos = options.teardownFunctions.mongos || [];
        assert(Array.isArray(options.teardownFunctions.mongos),
               'Expected teardownFunctions.mongos to be an array');
        assert(options.teardownFunctions.mongos.every(f => (typeof f === 'function')),
               'Expected teardownFunctions.mongos to be an array of functions');

        options.teardownFunctions.config = options.teardownFunctions.config || [];
        assert(Array.isArray(options.teardownFunctions.config),
               'Expected teardownFunctions.config to be an array');
        assert(options.teardownFunctions.config.every(f => (typeof f === 'function')),
               'Expected teardownFunctions.config to be an array of functions');

        options.useExistingConnectionAsSeed = options.useExistingConnectionAsSeed || false;
        assert.eq('boolean', typeof options.useExistingConnectionAsSeed);
    }

    function makeReplSetTestConfig(numReplSetNodes, firstNodeOnlyVote) {
        const REPL_SET_VOTING_LIMIT = 7;
        // Workaround for SERVER-26893 to specify when numReplSetNodes > REPL_SET_VOTING_LIMIT.
        var firstNodeNotVoting = firstNodeOnlyVote ? 1 : REPL_SET_VOTING_LIMIT;
        var rstConfig = [];
        for (var i = 0; i < numReplSetNodes; i++) {
            rstConfig[i] = {};
            if (i >= firstNodeNotVoting) {
                rstConfig[i].rsConfig = {priority: 0, votes: 0};
            }
        }
        return rstConfig;
    }

    var conn;
    var secondaryConns;

    var st;
    var rawST;  // The raw ShardingTest object for test suites not using resmoke fixtures.

    var initialized = false;
    var clusterStartTime;

    var _conns = {mongos: [], mongod: []};
    var nextConn = 0;
    var replSets = [];
    var rst;

    validateClusterOptions(options);
    Object.freeze(options);

    this.setup = function setup() {
        var verbosityLevel = 0;

        if (initialized) {
            throw new Error('cluster has already been initialized');
        }

        if (options.sharded.enabled) {
            if (options.useExistingConnectionAsSeed) {
                st = new FSMShardingTest(`mongodb://${db.getMongo().host}`);
            } else {
                // TODO: allow 'options' to specify the number of shards and mongos processes
                var shardConfig = {
                    shards: options.sharded.numShards,
                    mongos: options.sharded.numMongos,
                    verbose: verbosityLevel,
                    other: {
                        enableAutoSplit: options.sharded.enableAutoSplit,
                        enableBalancer: options.sharded.enableBalancer,
                    }
                };

                // TODO: allow 'options' to specify an 'rs' config
                if (options.replication.enabled) {
                    shardConfig.rs = {
                        nodes: makeReplSetTestConfig(options.replication.numNodes,
                                                     !this.shouldPerformContinuousStepdowns()),
                        // Increase the oplog size (in MB) to prevent rollover
                        // during write-heavy workloads
                        oplogSize: 1024,
                        // Set the electionTimeoutMillis to 1 day to prevent unintended elections
                        settings: {electionTimeoutMillis: 60 * 60 * 24 * 1000},
                        verbose: verbosityLevel
                    };
                    shardConfig.rsOptions = {};
                }

                if (this.shouldPerformContinuousStepdowns()) {
                    load('jstests/libs/override_methods/continuous_stepdown.js');
                    ContinuousStepdown.configure(options.sharded.stepdownOptions);
                }

                rawST = new ShardingTest(shardConfig);
                const hostStr = "mongodb://" + rawST._mongos.map(conn => conn.host).join(",");

                st = new FSMShardingTest(hostStr);
            }

            conn = st.s(0);  // First mongos

            this.teardown = function teardown(opts) {
                options.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);
                options.teardownFunctions.mongos.forEach(this.executeOnMongosNodes);
                options.teardownFunctions.config.forEach(this.executeOnConfigNodes);

                // Skip checking uuids in teardown if performing continuous stepdowns. The override
                // uses cached connections and expects to run commands against primaries, which is
                // not compatible with stepdowns.
                if (this.shouldPerformContinuousStepdowns()) {
                    TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
                }

                if (!options.useExistingConnectionAsSeed) {
                    rawST.stop(opts);
                }
            };

            if (this.shouldPerformContinuousStepdowns()) {
                this.startContinuousFailover = function() {
                    rawST.startContinuousFailover();
                };

                this.stopContinuousFailover = function() {
                    rawST.stopContinuousFailover(
                        {waitForPrimary: true, waitForMongosRetarget: true});

                    // Call getPrimary() to re-establish the connections in FSMShardingTest
                    // as it is not a transparent proxy for SharingTest/rawST.
                    st._configsvr.getPrimary();
                    for (let rst of st._shard_rsts) {
                        rst.getPrimary();
                    }
                };
            }

            // Save all mongos, mongod, and ReplSet connections (if any).
            var i;

            i = 0;
            while (st.s(i)) {
                _conns.mongos.push(st.s(i++));
            }

            i = 0;
            while (st.d(i)) {
                _conns.mongod.push(st.d(i++));
            }

            i = 0;
            while (st.rs(i)) {
                var rs = st.rs(i++);
                this._addReplicaSetConns(rs);
                replSets.push(rs);
            }

        } else if (options.replication.enabled) {
            var replSetConfig = {
                nodes: makeReplSetTestConfig(options.replication.numNodes,
                                             !this.shouldPerformContinuousStepdowns()),
                // Increase the oplog size (in MB) to prevent rollover during write-heavy workloads
                oplogSize: 1024,
                nodeOptions: {verbose: verbosityLevel},
                // Set the electionTimeoutMillis to 1 day to prevent unintended elections
                settings: {electionTimeoutMillis: 60 * 60 * 24 * 1000}
            };

            if (!options.useExistingConnectionAsSeed) {
                rst = new ReplSetTest(replSetConfig);
                rst.startSet();

                rst.initiate();
                rst.awaitSecondaryNodes();
            } else {
                rst = new ReplSetTest(db.getMongo().host);
            }

            conn = rst.getPrimary();
            secondaryConns = rst.getSecondaries();
            replSets = [rst];

            this.teardown = function teardown(opts) {
                options.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);
                if (!options.useExistingConnectionAsSeed) {
                    rst.stopSet(undefined, undefined, opts);
                }
            };

            this._addReplicaSetConns(rst);

        } else {  // standalone server
            conn = db.getMongo();
            db.adminCommand({setParameter: 1, logLevel: verbosityLevel});

            _conns.mongod = [conn];
        }

        initialized = true;
        clusterStartTime = new Date();

        options.setupFunctions.mongod.forEach(this.executeOnMongodNodes);
        options.setupFunctions.config.forEach(this.executeOnConfigNodes);
        if (options.sharded) {
            options.setupFunctions.mongos.forEach(this.executeOnMongosNodes);
        }
    };

    this._addReplicaSetConns = function _addReplicaSetConns(rsTest) {
        _conns.mongod.push(rsTest.getPrimary());
        rsTest.getSecondaries().forEach(function(secondaryConn) {
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
            fn(mongosConn.getDB('admin'), true /* isMongos */);
        });
    };

    this.executeOnConfigNodes = function executeOnConfigNodes(fn) {
        assert(initialized, 'cluster must be initialized first');

        if (!fn || typeof(fn) !== 'function' || fn.length !== 1) {
            throw new Error('config function must be a function that takes a db as an argument');
        }

        var configs = [];
        var config = st.c(0);
        var i = 0;
        while (config) {
            configs.push(config);
            ++i;
            config = st.c(i);
        }

        configs.forEach(function(conn) {
            fn(conn.getDB('admin'));
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

    this.getSecondaryHost = function getSecondaryHost(dbName) {
        assert(initialized, 'cluster must be initialized first');

        if (this.isReplication() && !this.isSharded()) {
            return secondaryConns[nextConn++ % secondaryConns.length].host;
        }
        return undefined;
    };

    this.getReplSetName = function getReplSetName() {
        if (this.isReplication() && !this.isSharded()) {
            return rst.name;
        }
        return undefined;
    };

    this.getReplSetNumNodes = function getReplSetNumNodes() {
        assert(this.isReplication() && !this.isSharded(), 'cluster must be a replica set');
        return options.replication.numNodes;
    };

    this.isSharded = function isSharded() {
        return Cluster.isSharded(options);
    };

    this.isReplication = function isReplication() {
        return Cluster.isReplication(options);
    };

    this.isStandalone = function isStandalone() {
        return Cluster.isStandalone(options);
    };

    this.shardCollection = function shardCollection() {
        assert(initialized, 'cluster must be initialized first');
        assert(this.isSharded(), 'cluster is not sharded');

        // If we are continuously stepping down shards, the config server may have stale view of the
        // cluster, so retry on retryable errors, e.g. NotMaster.
        if (this.shouldPerformContinuousStepdowns()) {
            assert.soon(() => {
                try {
                    st.shardColl.apply(st, arguments);
                    return true;
                } catch (e) {
                    // The shardCollection command requires the config server primary to call
                    // listCollections and listIndexes on shards before sharding the collection,
                    // both of which can fail with a retryable error if the config server's view of
                    // the cluster is stale. This is safe to retry because no actual work has been
                    // done.
                    //
                    // TODO SERVER-30949: Remove this try catch block once listCollections and
                    // listIndexes automatically retry on NotMaster errors.
                    if (e.code === 18630 ||  // listCollections failure
                        e.code === 18631) {  // listIndexes failure
                        print("Caught retryable error from shardCollection, retrying: " +
                              tojson(e));
                        return false;
                    }
                    throw e;
                }
            });
        }

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

        var cluster = {mongos: [], config: [], shards: {}};

        var i = 0;
        var mongos = st.s(0);
        while (mongos) {
            cluster.mongos.push(mongos.name);
            ++i;
            mongos = st.s(i);
        }

        i = 0;
        var config = st.c(0);
        while (config) {
            cluster.config.push(config.name);
            ++i;
            config = st.c(i);
        }

        i = 0;
        var shard = st.shard(0);
        while (shard) {
            if (shard.name.includes('/')) {
                // If the shard is a replica set, the format of st.shard(0).name in ShardingTest is
                // "test-rs0/localhost:20006,localhost:20007,localhost:20008".
                var [setName, shards] = shard.name.split('/');
                cluster.shards[setName] = shards.split(',');
            } else {
                // If the shard is a standalone mongod, the format of st.shard(0).name in
                // ShardingTest is "localhost:20006".
                cluster.shards[shard.shardName] = [shard.name];
            }
            ++i;
            shard = st.shard(i);
        }
        return cluster;
    };

    this.isBalancerEnabled = function isBalancerEnabled() {
        return this.isSharded() && options.sharded.enableBalancer;
    };

    this.isAutoSplitEnabled = function isAutoSplitEnabled() {
        return this.isSharded() && options.sharded.enableAutoSplit;
    };

    this.validateAllCollections = function validateAllCollections(phase) {
        assert(initialized, 'cluster must be initialized first');

        const isSteppingDownConfigServers = this.isSteppingDownConfigServers();
        var _validateCollections = function _validateCollections(db, isMongos = false) {
            // Validate all the collections on each node.
            var res = db.adminCommand({listDatabases: 1});
            assert.commandWorked(res);
            res.databases.forEach(dbInfo => {
                // Don't perform listCollections on the admin or config database through a mongos
                // connection when stepping down the config server primary, because both are stored
                // on the config server, and listCollections may return a not master error if the
                // mongos is stale.
                //
                // TODO SERVER-30949: listCollections through mongos should automatically retry on
                // NotMaster errors. Once that is true, remove this check.
                if (isSteppingDownConfigServers && isMongos &&
                    (dbInfo.name === "admin" || dbInfo.name === "config")) {
                    return;
                }

                if (!validateCollections(db.getSiblingDB(dbInfo.name), {full: true})) {
                    throw new Error(phase + ' collection validation failed');
                }
            });
        };

        var startTime = Date.now();
        jsTest.log('Starting to validate collections ' + phase);
        this.executeOnMongodNodes(_validateCollections);
        this.executeOnMongosNodes(_validateCollections);
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

            // Use '_master' instead of getPrimary() to avoid the detection of a new primary.
            var primary = rst._master;

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
            // shards since mongos won't report it.
            adminDB = st.shard(0).getDB('admin');
        }

        var res = adminDB.runCommand({getCmdLineOpts: 1});
        assert.commandWorked(res, 'failed to get command line options');

        var wiredTigerOptions = {};
        if (res.parsed && res.parsed.storage) {
            wiredTigerOptions = res.parsed.storage.wiredTiger || {};
        }
        var wiredTigerCollectionConfig = wiredTigerOptions.collectionConfig || {};
        var wiredTigerConfigString = wiredTigerCollectionConfig.configString || '';

        return wiredTigerConfigString === 'type=lsm';
    };

    this.shouldPerformContinuousStepdowns = function shouldPerformContinuousStepdowns() {
        return this.isSharded() && (typeof options.sharded.stepdownOptions !== 'undefined');
    };

    this.isSteppingDownConfigServers = function isSteppingDownConfigServers() {
        return this.shouldPerformContinuousStepdowns() &&
            options.sharded.stepdownOptions.configStepdown;
    };

    this.isSteppingDownShards = function isSteppingDownShards() {
        return this.shouldPerformContinuousStepdowns() &&
            options.sharded.stepdownOptions.shardStepdown;
    };
};

/**
 * Returns true if 'clusterOptions' represents a standalone mongod,
 * and false otherwise.
 */
Cluster.isStandalone = function isStandalone(clusterOptions) {
    return !clusterOptions.sharded.enabled && !clusterOptions.replication.enabled;
};

/**
 * Returns true if 'clusterOptions' represents a replica set, and returns false otherwise.
 */
Cluster.isReplication = function isReplication(clusterOptions) {
    return clusterOptions.replication.enabled;
};

/**
 * Returns true if 'clusterOptions' represents a sharded configuration, and returns false otherwise.
 */
Cluster.isSharded = function isSharded(clusterOptions) {
    return clusterOptions.sharded.enabled;
};
