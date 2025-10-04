/**
 * Represents a MongoDB cluster.
 */
import {FSMShardingTest} from "jstests/concurrency/fsm_libs/shard_fixture.js";
import {validateCollections} from "jstests/hooks/validate_collections.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

export const Cluster = function (clusterOptions, sessionOptions) {
    if (!(this instanceof Cluster)) {
        return new Cluster(clusterOptions);
    }

    function getObjectKeys(obj) {
        let values = [];
        if (typeof obj !== "object") {
            return values;
        }
        Object.keys(obj).forEach((key) => {
            if (key.indexOf(".") > -1) {
                throw new Error("illegal key specified " + key);
            }
            let subKeys = getObjectKeys(obj[key]);
            if (subKeys.length === 0) {
                values.push(key);
            } else {
                subKeys.forEach((subKey) => {
                    values.push(key + "." + subKey);
                });
            }
        });
        return values;
    }

    function validateClusterOptions(options) {
        let allowedKeys = [
            "replication.enabled",
            "replication.numNodes",
            "sameCollection",
            "sameDB",
            "setupFunctions",
            "sharded.enabled",
            "sharded.enableBalancer",
            "sharded.numMongos",
            "sharded.numShards",
            "sharded.stepdownOptions",
            "sharded.stepdownOptions.configStepdown",
            "sharded.stepdownOptions.shardStepdown",
            "teardownFunctions",
        ];

        getObjectKeys(options).forEach(function (option) {
            assert.contains(
                option,
                allowedKeys,
                "invalid option: " + tojson(option) + "; valid options are: " + tojson(allowedKeys),
            );
        });

        options.replication = options.replication || {};
        assert.eq("object", typeof options.replication);

        options.replication.enabled = options.replication.enabled || false;
        assert.eq("boolean", typeof options.replication.enabled);

        if (typeof options.replication.numNodes !== "undefined") {
            assert(
                options.replication.enabled,
                "Must have replication.enabled be true if 'replication.numNodes' is specified",
            );
        }

        options.replication.numNodes = options.replication.numNodes || 3;
        assert.eq("number", typeof options.replication.numNodes);

        options.sameCollection = options.sameCollection || false;
        assert.eq("boolean", typeof options.sameCollection);

        options.sameDB = options.sameDB || false;
        assert.eq("boolean", typeof options.sameDB);

        options.sharded = options.sharded || {};
        assert.eq("object", typeof options.sharded);

        options.sharded.enabled = options.sharded.enabled || false;
        assert.eq("boolean", typeof options.sharded.enabled);

        if (typeof options.sharded.enableBalancer !== "undefined") {
            assert(
                options.sharded.enabled,
                "Must have sharded.enabled be true if 'sharded.enableBalancer' is specified",
            );
        }

        options.sharded.enableBalancer = options.sharded.enableBalancer || false;
        assert.eq("boolean", typeof options.sharded.enableBalancer);

        if (typeof options.sharded.numMongos !== "undefined") {
            assert(options.sharded.enabled, "Must have sharded.enabled be true if 'sharded.numMongos' is specified");
        }

        options.sharded.numMongos = options.sharded.numMongos || 2;
        assert.eq("number", typeof options.sharded.numMongos);

        if (typeof options.sharded.numShards !== "undefined") {
            assert(options.sharded.enabled, "Must have sharded.enabled be true if 'sharded.numShards' is specified");
        }

        options.sharded.numShards = options.sharded.numShards || 2;
        assert.eq("number", typeof options.sharded.numShards);

        options.setupFunctions = options.setupFunctions || {};
        assert.eq("object", typeof options.setupFunctions);

        options.setupFunctions.mongod = options.setupFunctions.mongod || [];
        assert(Array.isArray(options.setupFunctions.mongod), "Expected setupFunctions.mongod to be an array");
        assert(
            options.setupFunctions.mongod.every((f) => typeof f === "function"),
            "Expected setupFunctions.mongod to be an array of functions",
        );

        if (typeof options.setupFunctions.mongos !== "undefined") {
            assert(
                options.sharded.enabled,
                "Must have sharded.enabled be true if 'setupFunctions.mongos' is specified",
            );
        }

        options.setupFunctions.mongos = options.setupFunctions.mongos || [];
        assert(Array.isArray(options.setupFunctions.mongos), "Expected setupFunctions.mongos to be an array");
        assert(
            options.setupFunctions.mongos.every((f) => typeof f === "function"),
            "Expected setupFunctions.mongos to be an array of functions",
        );

        options.setupFunctions.config = options.setupFunctions.config || [];
        assert(Array.isArray(options.setupFunctions.config), "Expected setupFunctions.config to be an array");
        assert(
            options.setupFunctions.config.every((f) => typeof f === "function"),
            "Expected setupFunctions.config to be an array of functions",
        );

        options.teardownFunctions = options.teardownFunctions || {};
        assert.eq("object", typeof options.teardownFunctions);

        options.teardownFunctions.mongod = options.teardownFunctions.mongod || [];
        assert(Array.isArray(options.teardownFunctions.mongod), "Expected teardownFunctions.mongod to be an array");
        assert(
            options.teardownFunctions.mongod.every((f) => typeof f === "function"),
            "Expected teardownFunctions.mongod to be an array of functions",
        );

        if (typeof options.teardownFunctions.mongos !== "undefined") {
            assert(
                options.sharded.enabled,
                "Must have sharded.enabled be true if 'teardownFunctions.mongos' is specified",
            );
        }

        options.teardownFunctions.mongos = options.teardownFunctions.mongos || [];
        assert(Array.isArray(options.teardownFunctions.mongos), "Expected teardownFunctions.mongos to be an array");
        assert(
            options.teardownFunctions.mongos.every((f) => typeof f === "function"),
            "Expected teardownFunctions.mongos to be an array of functions",
        );

        options.teardownFunctions.config = options.teardownFunctions.config || [];
        assert(Array.isArray(options.teardownFunctions.config), "Expected teardownFunctions.config to be an array");
        assert(
            options.teardownFunctions.config.every((f) => typeof f === "function"),
            "Expected teardownFunctions.config to be an array of functions",
        );
    }

    let conn;
    let session;
    let secondaryConns;

    let st;

    let initialized = false;
    let clusterStartTime;

    let _conns = {mongos: [], mongod: []};
    let nextConn = 0;
    let replSets = [];
    let rst;

    validateClusterOptions(clusterOptions);
    Object.freeze(clusterOptions);

    this.setup = async function setup() {
        let verbosityLevel = 0;

        if (initialized) {
            throw new Error("cluster has already been initialized");
        }

        if (clusterOptions.sharded.enabled) {
            st = new FSMShardingTest(`mongodb://${db.getMongo().host}`);

            conn = st.s(0); // First mongos

            this.teardown = function teardown() {
                clusterOptions.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);
                clusterOptions.teardownFunctions.mongos.forEach(this.executeOnMongosNodes);
                clusterOptions.teardownFunctions.config.forEach(this.executeOnConfigNodes);
            };

            this.reestablishConnectionsAfterFailover = function () {
                // Call getPrimary() to re-establish the connections in FSMShardingTest
                // as it is not a transparent proxy for ShardingTest.
                st._configsvr.getPrimary();
                for (let rst of st._shard_rsts) {
                    rst.getPrimary();
                }
            };

            // Save all mongos, mongod, and ReplSet connections (if any).
            let i;

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
                const rs = st.rs(i++);
                this._addReplicaSetConns(rs);
                replSets.push(rs);
            }
        } else if (clusterOptions.replication.enabled) {
            rst = new ReplSetTest(db.getMongo().host);

            conn = rst.getPrimary();
            secondaryConns = rst.getSecondaries();
            replSets = [rst];

            this.teardown = function teardown() {
                clusterOptions.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);
            };

            this._addReplicaSetConns(rst);
        } else {
            // standalone server
            conn = db.getMongo();
            db.adminCommand({setParameter: 1, logLevel: verbosityLevel});

            _conns.mongod = [conn];
        }

        if (typeof sessionOptions !== "undefined") {
            session = conn.startSession(sessionOptions);
            const readPreference = session.getOptions().getReadPreference();
            if (readPreference && readPreference.mode === "secondary") {
                // Unset the explicit read preference so set_read_preference_secondary.js can do
                // the right thing based on the DB.
                session.getOptions().setReadPreference(undefined);

                // We import set_read_preference_secondary.js in order to avoid running
                // commands against the "admin" and "config" databases via mongos with
                // readPreference={mode: "secondary"} when there's only a single node in
                // the CSRS.
                await import("jstests/libs/override_methods/set_read_preference_secondary.js");
            }
        }

        initialized = true;
        clusterStartTime = new Date();

        clusterOptions.setupFunctions.mongod.forEach(this.executeOnMongodNodes);
        clusterOptions.setupFunctions.config.forEach(this.executeOnConfigNodes);
        if (clusterOptions.sharded) {
            clusterOptions.setupFunctions.mongos.forEach(this.executeOnMongosNodes);
        }
    };

    this._addReplicaSetConns = function _addReplicaSetConns(rsTest) {
        _conns.mongod.push(rsTest.getPrimary());
        rsTest.getSecondaries().forEach(function (secondaryConn) {
            _conns.mongod.push(secondaryConn);
        });
    };

    this.executeOnMongodNodes = function executeOnMongodNodes(fn) {
        assert(initialized, "cluster must be initialized first");

        if (!fn || typeof fn !== "function" || fn.length !== 1) {
            throw new Error("mongod function must be a function that takes a db as an argument");
        }

        if (TestData.shardsAddedRemoved) {
            _conns.mongod.forEach(function (mongodConn) {
                try {
                    fn(mongodConn.getDB("admin"));
                } catch (e) {
                    print(
                        "Error executing function on mongod nodes, with addShard/removeShard running in background: " +
                            tojson(e),
                    );
                }
            });
        } else {
            _conns.mongod.forEach(function (mongodConn) {
                fn(mongodConn.getDB("admin"));
            });
        }
    };

    this.executeOnMongosNodes = function executeOnMongosNodes(fn) {
        assert(initialized, "cluster must be initialized first");

        if (!fn || typeof fn !== "function" || fn.length !== 1) {
            throw new Error("mongos function must be a function that takes a db as an argument");
        }
        _conns.mongos.forEach(function (mongosConn) {
            fn(mongosConn.getDB("admin"), true /* isMongos */);
        });
    };

    this.executeOnConfigNodes = function executeOnConfigNodes(fn) {
        assert(initialized, "cluster must be initialized first");

        if (!fn || typeof fn !== "function" || fn.length !== 1) {
            throw new Error("config function must be a function that takes a db as an argument");
        }
        st._configServers.forEach(function (conn) {
            fn(conn.getDB("admin"));
        });
    };

    this.getConfigPrimaryNode = function getConfigPrimaryNode() {
        assert(initialized, "cluster must be initialized first");
        return st._configsvr.getPrimary();
    };

    this.synchronizeMongosClusterTimes = function synchronizeMongosClusterTimes() {
        const contactConfigServerFn = (mongosConn) => {
            // The admin database is hosted on the config server.
            assert.commandWorked(mongosConn.adminCommand({find: "foo"}));
        };

        // After the first iteration, the config server will have been gossiped the highest cluster
        // time any mongos has seen. After the second iteration, each mongos should have been
        // gossiped this time as well.
        this.executeOnMongosNodes(contactConfigServerFn);
        this.executeOnMongosNodes(contactConfigServerFn);
    };

    this.teardown = function teardown() {
        assert(initialized, "cluster must be initialized first");
        clusterOptions.teardownFunctions.mongod.forEach(this.executeOnMongodNodes);
    };

    this.getDB = function getDB(dbName) {
        assert(initialized, "cluster must be initialized first");
        if (session) {
            return session.getDatabase(dbName);
        }
        return conn.getDB(dbName);
    };

    this.getHost = function getHost() {
        assert(initialized, "cluster must be initialized first");

        // Alternate mongos connections for sharded clusters
        if (this.isSharded()) {
            return _conns.mongos[nextConn++ % _conns.mongos.length].host;
        }
        return conn.host;
    };

    this.getSecondaryHost = function getSecondaryHost(dbName) {
        assert(initialized, "cluster must be initialized first");

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
        assert(this.isReplication() && !this.isSharded(), "cluster must be a replica set");
        return clusterOptions.replication.numNodes;
    };

    this.isSharded = function isSharded() {
        return Cluster.isSharded(clusterOptions);
    };

    this.isReplication = function isReplication() {
        return Cluster.isReplication(clusterOptions);
    };

    this.isStandalone = function isStandalone() {
        return Cluster.isStandalone(clusterOptions);
    };

    this.shardCollection = function shardCollection() {
        assert(initialized, "cluster must be initialized first");
        assert(this.isSharded(), "cluster is not sharded");

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
        assert(initialized, "cluster must be initialized first");

        // TODO: Add support for non-sharded clusters.
        if (!this.isSharded()) {
            return "";
        }

        let cluster = {mongos: [], config: [], shards: {}};

        let i = 0;
        let mongos = st.s(0);
        while (mongos) {
            cluster.mongos.push(mongos.name);
            ++i;
            mongos = st.s(i);
        }

        i = 0;
        let config = st.c(0);
        while (config) {
            cluster.config.push(config.name);
            ++i;
            config = st.c(i);
        }

        i = 0;
        let shard = st.shard(0);
        while (shard) {
            if (TestData.shardsAddedRemoved && shard.shardName === "config") {
                // Skip the config shard if it's transitioning in and out of being a shard to avoid
                // tests that use manual shard operations failing with ShardNotFound.
            } else if (shard.name.includes("/")) {
                // If the shard is a replica set, the format of st.shard(0).name in ShardingTest is
                // "test-rs0/localhost:20006,localhost:20007,localhost:20008".
                let [_, shards] = shard.name.split("/");
                cluster.shards[shard.shardName] = shards.split(",");
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

    this.getReplicaSets = function getReplicaSets() {
        assert(initialized, "cluster must be initialized first");
        assert(this.isReplication() || this.isSharded());
        return replSets;
    };

    this.isBalancerEnabled = function isBalancerEnabled() {
        return this.isSharded() && clusterOptions.sharded.enableBalancer;
    };

    this.validateAllCollections = function validateAllCollections(phase) {
        assert(initialized, "cluster must be initialized first");

        let _validateCollections = function _validateCollections(db, isMongos = false) {
            // Validate all the collections on each node.
            let res = db.adminCommand({listDatabases: 1});
            assert.commandWorked(res);
            res.databases.forEach((dbInfo) => {
                const validateOptions = {full: true, enforceFastCount: true};
                // TODO (SERVER-24266): Once fast counts are tolerant to unclean shutdowns, remove
                // the check for TestData.allowUncleanShutdowns.
                if (TestData.skipEnforceFastCountOnValidate || TestData.allowUncleanShutdowns) {
                    validateOptions.enforceFastCount = false;
                }

                assert.commandWorked(
                    validateCollections(db.getSiblingDB(dbInfo.name), validateOptions),
                    phase + " collection validation failed",
                );
            });
        };

        let startTime = Date.now();
        jsTest.log("Starting to validate collections " + phase);
        this.executeOnMongodNodes(_validateCollections);
        this.executeOnMongosNodes(_validateCollections);
        let totalTime = Date.now() - startTime;
        jsTest.log("Finished validating collections in " + totalTime + " ms, " + phase);
    };

    this.checkReplicationConsistency = function checkReplicationConsistency(dbDenylist, phase) {
        assert(initialized, "cluster must be initialized first");

        if (!this.isReplication()) {
            return;
        }

        let shouldCheckDBHashes = !this.isBalancerEnabled();

        replSets.forEach((rst) => {
            let startTime = Date.now();
            let primary = rst.getPrimary();

            if (shouldCheckDBHashes) {
                jsTest.log(
                    "Starting consistency checks for replica set with " +
                        primary.host +
                        " assumed to still be primary, " +
                        phase,
                );

                // Compare the dbhashes of the primary and secondaries.
                rst.checkReplicatedDataHashes(phase, dbDenylist);
                let totalTime = Date.now() - startTime;
                jsTest.log(
                    "Finished consistency checks of replica set with " +
                        primary.host +
                        " as primary in " +
                        totalTime +
                        " ms, " +
                        phase,
                );
            } else {
                jsTest.log(
                    "Skipping consistency checks when the balancer is enabled, " +
                        "for replica set with " +
                        primary.host +
                        " assumed to still be primary, " +
                        phase,
                );

                // Get the latest optime from the primary.
                let replSetStatus = primary.adminCommand({replSetGetStatus: 1});
                assert.commandWorked(replSetStatus, phase + ", error getting replication status");

                let primaryInfo = replSetStatus.members.find((memberInfo) => memberInfo.self);
                assert(
                    primaryInfo !== undefined,
                    phase + ", failed to find self in replication status: " + tojson(replSetStatus),
                );

                // Wait for all previous workload operations to complete.
                rst.awaitReplication();
            }
        });
    };

    this.isRunningWiredTigerLSM = function isRunningWiredTigerLSM() {
        let adminDB = this.getDB("admin");

        if (this.isSharded()) {
            // Get the storage engine the sharded cluster is configured to use from one of the
            // shards since mongos won't report it.
            adminDB = st.shard(0).getDB("admin");
        }

        let res = adminDB.runCommand({getCmdLineOpts: 1});
        assert.commandWorked(res, "failed to get command line options");

        let wiredTigerOptions = {};
        if (res.parsed && res.parsed.storage) {
            wiredTigerOptions = res.parsed.storage.wiredTiger || {};
        }
        let wiredTigerCollectionConfig = wiredTigerOptions.collectionConfig || {};
        let wiredTigerConfigString = wiredTigerCollectionConfig.configString || "";

        return wiredTigerConfigString === "type=lsm";
    };

    this.shouldPerformContinuousStepdowns = function shouldPerformContinuousStepdowns() {
        return this.isSharded() && typeof clusterOptions.sharded.stepdownOptions !== "undefined";
    };

    /*
     * Returns true if this cluster has a config shard.
     * Config shard always have shard ID equal to "config".
     */
    this.hasConfigShard = function hasConfigShard() {
        if (!this.isSharded()) {
            return false;
        }
        let i = 0;
        while (st.shard(i)) {
            if (st.shard(i++).shardName === "config") return true;
        }
        return false;
    };

    this.isSteppingDownConfigServers = function isSteppingDownConfigServers() {
        return this.shouldPerformContinuousStepdowns() && clusterOptions.sharded.stepdownOptions.configStepdown;
    };

    this.isSteppingDownShards = function isSteppingDownShards() {
        return this.shouldPerformContinuousStepdowns() && clusterOptions.sharded.stepdownOptions.shardStepdown;
    };

    this.awaitReplication = () => {
        assert(this.isReplication(), "cluster does not contain replica sets");
        for (let rst of replSets) {
            rst.awaitReplication();
        }
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
