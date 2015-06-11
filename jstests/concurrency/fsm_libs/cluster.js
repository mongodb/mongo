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
            'masterSlave',
            'replication',
            'sameCollection',
            'sameDB',
            'setupFunctions',
            'sharded',
            ];

        Object.keys(options).forEach(function(option) {
            assert.contains(option, allowedKeys,
                'invalid option: ' + tojson(option) +
                '; valid options are: ' + tojson(allowedKeys));
        });

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

        options.setupFunctions = options.setupFunctions || {};
        assert.eq('object', typeof options.setupFunctions);

        options.setupFunctions.mongod = options.setupFunctions.mongod || function(db) { };
        assert.eq('function', typeof options.setupFunctions.mongod);

        options.setupFunctions.mongos = options.setupFunctions.mongos || function(db) { };
        assert.eq('function', typeof options.setupFunctions.mongos);

        assert(!options.masterSlave || !options.replication, "Both 'masterSlave' and " +
               "'replication' cannot be true");
        assert(!options.masterSlave || !options.sharded, "Both 'masterSlave' and 'sharded' cannot" +
               "be true");
    }

    var conn;

    var initialized = false;

    var _conns = {
        mongos: [],
        mongod: []
    };
    var nextConn = 0;

    validateClusterOptions(options);
    Object.freeze(options);

    this.setup = function setup() {
        var verbosityLevel = 0;

        if (initialized) {
            throw new Error('cluster has already been initialized');
        }

        if (options.sharded) {
            // TODO: allow 'options' to specify the number of shards and mongos processes
            var shardConfig = {
                shards: 2,
                mongos: 2,
                verbose: verbosityLevel
            };

            // TODO: allow 'options' to specify an 'rs' config
            if (options.replication) {
                shardConfig.rs = {
                    nodes: 3,
                    // Increase the oplog size (in MB) to prevent rollover
                    // during write-heavy workloads
                    oplogSize: 1024,
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
                nodes: 3,
                // Increase the oplog size (in MB) to prevent rollover during write-heavy workloads
                oplogSize: 1024,
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

            this._addReplicaSetConns(rst);

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

            _conns.mongod = [master, slave];

        } else { // standalone server
            conn = db.getMongo();
            db.adminCommand({ setParameter: 1, logLevel: verbosityLevel });

            _conns.mongod = [conn];
        }

        initialized = true;
        
        this.executeOnMongodNodes(options.setupFunctions.mongod);
        this.executeOnMongosNodes(options.setupFunctions.mongos);
    };


    this._addReplicaSetConns = function _addReplicaSetConns(rsTest) {
        _conns.mongod.push(rsTest.getPrimary());
        rsTest.getSecondaries().forEach(function (secondaryConn) {
            _conns.mongod.push(secondaryConn);
        });
    };

    this.executeOnMongodNodes = function executeOnMongodNodes(fn) {
        if (!initialized) {
            throw new Error('cluster must be initialized before functions can be executed ' +
                            'against it');
        }
        if (!fn || typeof(fn) !== 'function' || fn.length !== 1) {
            throw new Error('mongod function must be a function that takes a db as an argument');
        } 
        _conns.mongod.forEach(function(mongodConn) {
            fn(mongodConn.getDB('admin'));
        });
    };
        
    this.executeOnMongosNodes = function executeOnMongosNodes(fn) {
        if (!initialized) {
            throw new Error('cluster must be initialized before functions can be executed ' +
                            'against it');
        }
        if (!fn || typeof(fn) !== 'function' || fn.length !== 1) {
            throw new Error('mongos function must be a function that takes a db as an argument');
        } 
        _conns.mongos.forEach(function(mongosConn) {
            fn(mongosConn.getDB('admin'));
        });
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

        // Alternate mongos connections for sharded clusters
        if (this.isSharded()) {
            return _conns.mongos[nextConn++ % _conns.mongos.length].host;
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
