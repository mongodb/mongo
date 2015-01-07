'use strict';

/**
 * Represents a MongoDB cluster.
 */

var Cluster = function(options) {
    if (!(this instanceof Cluster)) {
        return new Cluster(options);
    }

    var allowedKeys = [
        'masterSlave',
        'replication',
        'sameCollection',
        'sameDB',
        'seed',
        'sharded'
    ];

    Object.keys(options).forEach(function(option) {
        assert.contains(option, allowedKeys,
                        'invalid option: ' + tojson(option) +
                        '; valid options are: ' + tojson(allowedKeys));
    });

    var conn;

    var initialized = false;

    this.setup = function setup() {
        var verbosityLevel = 0;

        if (initialized) {
            throw new Error('cluster has already been initialized');
        }

        if (options.sharded) {
            // TODO: allow 'options' to specify the number of shards
            var shardConfig = {
                shards: 2,
                mongos: 1,
                verbose: verbosityLevel
            };

            // TODO: allow 'options' to specify an 'rs' config
            if (options.replication) {
                shardConfig.rs = {
                    nodes: 3,
                    verbose: verbosityLevel
                };
            }

            var st = new ShardingTest(shardConfig);
            st.stopBalancer();

            conn = st.s; // mongos

            this.shardCollection = function() {
                st.shardColl.apply(st, arguments);
            };

            this.teardown = function() {
                st.stop();
            };

        } else if (options.replication) {
            // TODO: allow 'options' to specify the number of nodes
            var replSetConfig = {
                nodes: 3,
                nodeOptions: { verbose: verbosityLevel }
            };

            var rst = new ReplSetTest(replSetConfig);
            rst.startSet();

            // Send the replSetInitiate command and wait for initiation
            rst.initiate();
            rst.awaitSecondaryNodes();

            conn = rst.getPrimary();

            this.teardown = function() {
                rst.stopSet();
            };

        } else if (options.masterSlave) {
            var rt = new ReplTest('replTest');

            var master = rt.start(true);
            var slave = rt.start(false);
            conn = master;

            master.adminCommand({ setParameter: 1, logLevel: verbosityLevel });
            slave.adminCommand({ setParameter: 1, logLevel: verbosityLevel });

            this.teardown = function() {
                rt.stop();
            };

        } else { // standalone server
            conn = db.getMongo();
            db.adminCommand({ setParameter: 1, logLevel: verbosityLevel });
        }

        initialized = true;
    };

    this.teardown = function teardown() { };

    this.getDB = function getDB(dbName) {
        if (!initialized) {
            throw new Error('cluster has not been initialized yet');
        }

        return conn.getDB(dbName);
    };

    this.getHost = function getHost() {
        if (!initialized) {
            throw new Error('cluster has not been initialized yet');
        }

        return conn.host;
    };

    this.isSharded = function isSharded() {
        return !!options.sharded;
    };

    this.shardCollection = function shardCollection() {
        assert(this.isSharded(), 'cluster is not sharded');
        throw new Error('cluster has not been initialized yet');
    };
};

/**
 * Returns true if 'clusterOptions' represents a standalone mongod,
 * and false otherwise.
 */
Cluster.isStandalone = function isStandalone(clusterOptions) {
    return !clusterOptions.sharded && !clusterOptions.replication && !clusterOptions.masterSlave;
};
