/**
 * Starts up a sharded cluster with the given specifications. The cluster will be fully operational
 * after the execution of this constructor function.
 *
 * In addition to its own methods, ShardingTest inherits all the functions from the 'sh' utility
 * with the db set as the first mongos instance in the test (i.e. s0).
 *
 * @param {Object} params Contains the key-value pairs for the cluster
 *   configuration. Accepted keys are:
 *
 *   {
 *     name {string}: name for this test
 *     verbose {number}: the verbosity for the mongos
 *     chunkSize {number}: the chunk size to use as configuration for the cluster
 *     nopreallocj {boolean|number}:
 *
 *     mongos {number|Object|Array.<Object>}: number of mongos or mongos
 *       configuration object(s)(*). @see MongoRunner.runMongos
 *
 *     rs {Object|Array.<Object>}: replica set configuration object. Can
 *       contain:
 *       {
 *         nodes {number}: number of replica members. Defaults to 3.
 *         protocolVersion {number}: protocol version of replset used by the
 *             replset initiation.
 *         initiateTimeout {number}: timeout in milliseconds to specify
 *              to ReplSetTest.prototype.initiate().
 *         For other options, @see ReplSetTest#start
 *       }
 *
 *     shards {number|Object|Array.<Object>}: number of shards or shard
 *       configuration object(s)(*). @see MongoRunner.runMongod
 *
 *     config {number|Object|Array.<Object>}: number of config server or
 *       config server configuration object(s)(*). @see MongoRunner.runMongod
 *
 *     (*) There are two ways For multiple configuration objects.
 *       (1) Using the object format. Example:
 *
 *           { d0: { verbose: 5 }, d1: { auth: '' }, rs2: { oplogsize: 10 }}
 *
 *           In this format, d = mongod, s = mongos & c = config servers
 *
 *       (2) Using the array format. Example:
 *
 *           [{ verbose: 5 }, { auth: '' }]
 *
 *       Note: you can only have single server shards for array format.
 *
 *       Note: A special "bridgeOptions" property can be specified in both the object and array
 *          formats to configure the options for the mongobridge corresponding to that node. These
 *          options are merged with the params.bridgeOptions options, where the node-specific
 *          options take precedence.
 *
 *     other: {
 *       nopreallocj: same as above
 *       rs: same as above
 *       chunkSize: same as above
 *       keyFile {string}: the location of the keyFile
 *
 *       shardOptions {Object}: same as the shards property above.
 *          Can be used to specify options that are common all shards.
 *
 *       configOptions {Object}: same as the config property above.
 *          Can be used to specify options that are common all config servers.
 *       mongosOptions {Object}: same as the mongos property above.
 *          Can be used to specify options that are common all mongos.
 *       enableBalancer {boolean} : if true, enable the balancer
 *       manualAddShard {boolean}: shards will not be added if true.
 *
 *       useBridge {boolean}: If true, then a mongobridge process is started for each node in the
 *          sharded cluster. Defaults to false.
 *
 *       bridgeOptions {Object}: Options to apply to all mongobridge processes. Defaults to {}.
 *
 *       // replica Set only:
 *       rsOptions {Object}: same as the rs property above. Can be used to
 *         specify options that are common all replica members.
 *       useHostname {boolean}: if true, use hostname of machine,
 *         otherwise use localhost
 *       numReplicas {number},
 *       waitForCSRSSecondaries {boolean}: if false, will not wait for the read committed view
 *         of the secondaries to catch up with the primary. Defaults to true.
 *     }
 *   }
 *
 * Member variables:
 * s {Mongo} - connection to the first mongos
 * s0, s1, ... {Mongo} - connection to different mongos
 * rs0, rs1, ... {ReplSetTest} - test objects to replica sets
 * shard0, shard1, ... {Mongo} - connection to shards (not available for replica sets)
 * d0, d1, ... {Mongo} - same as shard0, shard1, ...
 * config0, config1, ... {Mongo} - connection to config servers
 * c0, c1, ... {Mongo} - same as config0, config1, ...
 * configRS - If the config servers are a replset, this will contain the config ReplSetTest object
 */
var ShardingTest = function(params) {

    if (!(this instanceof ShardingTest)) {
        return new ShardingTest(params);
    }

    // Capture the 'this' reference
    var self = this;

    // Used for counting the test duration
    var _startTime = new Date();

    // Populated with the paths of all shard hosts (config servers + hosts) and is used for
    // cleaning up the data files on shutdown
    var _alldbpaths = [];

    // Publicly exposed variables

    /**
     * Attempts to open a connection to the specified connection string or throws if unable to
     * connect.
     */
    function _connectWithRetry(url) {
        var conn;
        assert.soon(function() {
            try {
                conn = new Mongo(url);
                return true;
            } catch (e) {
                print("Error connecting to " + url + ": " + e);
                return false;
            }
        });

        return conn;
    }

    /**
     * Constructs a human-readable string representing a chunk's range.
     */
    function _rangeToString(r) {
        return tojsononeline(r.min) + " -> " + tojsononeline(r.max);
    }

    /**
     * Checks whether the specified collection is sharded by consulting the config metadata.
     */
    function _isSharded(collName) {
        var collName = "" + collName;
        var dbName;

        if (typeof collName.getCollectionNames == 'function') {
            dbName = "" + collName;
            collName = undefined;
        }

        if (dbName) {
            var x = self.config.databases.findOne({_id: dbname});
            if (x)
                return x.partitioned;
            else
                return false;
        }

        if (collName) {
            var x = self.config.collections.findOne({_id: collName});
            if (x)
                return true;
            else
                return false;
        }
    }

    /**
     * Extends the ShardingTest class with the methods exposed by the sh utility class.
     */
    function _extendWithShMethods() {
        Object.keys(sh).forEach(function(fn) {
            if (typeof sh[fn] !== 'function') {
                return;
            }

            assert.eq(undefined,
                      self[fn],
                      'ShardingTest contains a method ' + fn +
                          ' which duplicates a method with the same name on sh. ' +
                          'Please select a different function name.');

            self[fn] = function() {
                if (typeof db == "undefined") {
                    db = undefined;
                }

                var oldDb = db;
                db = self.getDB('test');

                try {
                    sh[fn].apply(sh, arguments);
                } finally {
                    db = oldDb;
                }
            };
        });
    }

    /**
     * Configures the cluster based on the specified parameters (balancer state, etc).
     */
    function _configureCluster() {
        // Disable the balancer unless it is explicitly turned on
        if (!otherParams.enableBalancer) {
            self.stopBalancer();
        }

        // Lower the mongos replica set monitor's threshold for deeming RS shard hosts as
        // inaccessible in order to speed up tests, which shutdown entire shards and check for
        // errors. This attempt is best-effort and failure should not have effect on the actual
        // test execution, just the execution time.
        self._mongos.forEach(function(mongos) {
            var res = mongos.adminCommand({setParameter: 1, replMonitorMaxFailedChecks: 2});

            // For tests, which use x509 certificate for authentication, the command above will not
            // work due to authorization error.
            if (res.code != ErrorCodes.Unauthorized) {
                assert.commandWorked(res);
            }
        });
    }

    function connectionURLTheSame(a, b) {
        if (a == b)
            return true;

        if (!a || !b)
            return false;

        if (a.host)
            return connectionURLTheSame(a.host, b);
        if (b.host)
            return connectionURLTheSame(a, b.host);

        if (a.name)
            return connectionURLTheSame(a.name, b);
        if (b.name)
            return connectionURLTheSame(a, b.name);

        if (a.indexOf("/") < 0 && b.indexOf("/") < 0) {
            a = a.split(":");
            b = b.split(":");

            if (a.length != b.length)
                return false;

            if (a.length == 2 && a[1] != b[1])
                return false;

            if (a[0] == "localhost" || a[0] == "127.0.0.1")
                a[0] = getHostName();
            if (b[0] == "localhost" || b[0] == "127.0.0.1")
                b[0] = getHostName();

            return a[0] == b[0];
        } else {
            var a0 = a.split("/")[0];
            var b0 = b.split("/")[0];
            return a0 == b0;
        }
    }

    assert(connectionURLTheSame("foo", "foo"));
    assert(!connectionURLTheSame("foo", "bar"));

    assert(connectionURLTheSame("foo/a,b", "foo/b,a"));
    assert(!connectionURLTheSame("foo/a,b", "bar/a,b"));

    // ShardingTest API

    this.getDB = function(name) {
        return this.s.getDB(name);
    };

    /**
     * Finds the _id of the primary shard for database 'dbname', e.g., 'test-rs0'
     */
    this.getPrimaryShardIdForDatabase = function(dbname) {
        var x = this.config.databases.findOne({_id: "" + dbname});
        if (x) {
            return x.primary;
        }

        var countDBsFound = 0;
        this.config.databases.find().forEach(function(db) {
            countDBsFound++;
            printjson(db);
        });
        throw Error("couldn't find dbname: " + dbname + " in config.databases. Total DBs: " +
                    countDBsFound);
    };

    this.getNonPrimaries = function(dbname) {
        var x = this.config.databases.findOne({_id: dbname});
        if (!x) {
            this.config.databases.find().forEach(printjson);
            throw Error("couldn't find dbname: " + dbname + " total: " +
                        this.config.databases.count());
        }

        return this.config.shards.find({_id: {$ne: x.primary}}).map(z => z._id);
    };

    this.getConnNames = function() {
        var names = [];
        for (var i = 0; i < this._connections.length; i++) {
            names.push(this._connections[i].name);
        }
        return names;
    };

    /**
     * Find the connection to the primary shard for database 'dbname'.
     */
    this.getPrimaryShard = function(dbname) {
        var dbPrimaryShardId = this.getPrimaryShardIdForDatabase(dbname);
        var primaryShard = this.config.shards.findOne({_id: dbPrimaryShardId});

        if (primaryShard) {
            shardConnectionString = primaryShard.host;
            var rsName = shardConnectionString.substring(0, shardConnectionString.indexOf("/"));

            for (var i = 0; i < this._connections.length; i++) {
                var c = this._connections[i];
                if (connectionURLTheSame(shardConnectionString, c.name) ||
                    connectionURLTheSame(rsName, c.name))
                    return c;
            }
        }

        throw Error("can't find server connection for db '" + dbname + "'s primary shard: " +
                    tojson(primaryShard));
    };

    this.normalize = function(x) {
        var z = this.config.shards.findOne({host: x});
        if (z)
            return z._id;
        return x;
    };

    /**
     * Find a different shard connection than the one given.
     */
    this.getOther = function(one) {
        if (this._connections.length < 2) {
            throw Error("getOther only works with 2 shards");
        }

        if (one._mongo) {
            one = one._mongo;
        }

        for (var i = 0; i < this._connections.length; i++) {
            if (this._connections[i] != one) {
                return this._connections[i];
            }
        }

        return null;
    };

    this.getAnother = function(one) {
        if (this._connections.length < 2) {
            throw Error("getAnother() only works with multiple servers");
        }

        if (one._mongo) {
            one = one._mongo;
        }

        for (var i = 0; i < this._connections.length; i++) {
            if (this._connections[i] == one)
                return this._connections[(i + 1) % this._connections.length];
        }
    };

    this.stop = function(opts) {
        for (var i = 0; i < this._mongos.length; i++) {
            this.stopMongos(i, opts);
        }

        for (var i = 0; i < this._connections.length; i++) {
            if (this._rs[i]) {
                this._rs[i].test.stopSet(15, undefined, opts);
            } else {
                this.stopMongod(i, opts);
            }
        }

        if (this.configRS) {
            this.configRS.stopSet(undefined, undefined, opts);
        } else {
            // Old style config triplet
            for (var i = 0; i < this._configServers.length; i++) {
                this.stopConfigServer(i, opts);
            }
        }

        for (var i = 0; i < _alldbpaths.length; i++) {
            resetDbpath(MongoRunner.dataPath + _alldbpaths[i]);
        }

        var timeMillis = new Date().getTime() - _startTime.getTime();

        print('*** ShardingTest ' + this._testName + " completed successfully in " +
              (timeMillis / 1000) + " seconds ***");
    };

    this.getDBPaths = function() {
        return _alldbpaths.map((path) => {
            return MongoRunner.dataPath + path;
        });
    };

    this.adminCommand = function(cmd) {
        var res = this.admin.runCommand(cmd);
        if (res && res.ok == 1)
            return true;

        throw _getErrorWithCode(res, "command " + tojson(cmd) + " failed: " + tojson(res));
    };

    this.printChangeLog = function() {
        this.config.changelog.find().forEach(function(z) {
            var msg = z.server + "\t" + z.time + "\t" + z.what;
            for (var i = z.what.length; i < 15; i++)
                msg += " ";

            msg += " " + z.ns + "\t";
            if (z.what == "split") {
                msg += _rangeToString(z.details.before) + " -->> (" +
                    _rangeToString(z.details.left) + "), (" + _rangeToString(z.details.right) + ")";
            } else if (z.what == "multi-split") {
                msg += _rangeToString(z.details.before) + "  -->> (" + z.details.number + "/" +
                    z.details.of + " " + _rangeToString(z.details.chunk) + ")";
            } else {
                msg += tojsononeline(z.details);
            }

            print("ShardingTest " + msg);
        });
    };

    this.getChunksString = function(ns) {
        var q = {};
        if (ns) {
            q.ns = ns;
        }

        var s = "";
        this.config.chunks.find(q).sort({ns: 1, min: 1}).forEach(function(z) {
            s += "  " + z._id + "\t" + z.lastmod.t + "|" + z.lastmod.i + "\t" + tojson(z.min) +
                " -> " + tojson(z.max) + " " + z.shard + "  " + z.ns + "\n";
        });

        return s;
    };

    this.printChunks = function(ns) {
        print("ShardingTest " + this.getChunksString(ns));
    };

    this.printShardingStatus = function(verbose) {
        printShardingStatus(this.config, verbose);
    };

    this.printCollectionInfo = function(ns, msg) {
        var out = "";
        if (msg) {
            out += msg + "\n";
        }
        out += "sharding collection info: " + ns + "\n";

        for (var i = 0; i < this._connections.length; i++) {
            var c = this._connections[i];
            out += "  mongod " + c + " " +
                tojson(c.getCollection(ns).getShardVersion(), " ", true) + "\n";
        }

        for (var i = 0; i < this._mongos.length; i++) {
            var c = this._mongos[i];
            out += "  mongos " + c + " " +
                tojson(c.getCollection(ns).getShardVersion(), " ", true) + "\n";
        }

        out += this.getChunksString(ns);

        print("ShardingTest " + out);
    };

    this.sync = function() {
        this.adminCommand("connpoolsync");
    };

    this.onNumShards = function(collName, dbName) {
        dbName = dbName || "test";

        // We should sync since we're going directly to mongod here
        this.sync();

        var num = 0;
        for (var i = 0; i < this._connections.length; i++) {
            if (this._connections[i].getDB(dbName).getCollection(collName).count() > 0) {
                num++;
            }
        }

        return num;
    };

    this.shardCounts = function(collName, dbName) {
        dbName = dbName || "test";

        // We should sync since we're going directly to mongod here
        this.sync();

        var counts = {};
        for (var i = 0; i < this._connections.length; i++) {
            counts[i] = this._connections[i].getDB(dbName).getCollection(collName).count();
        }

        return counts;
    };

    this.chunkCounts = function(collName, dbName) {
        dbName = dbName || "test";

        var x = {};
        this.config.shards.find().forEach(function(z) {
            x[z._id] = 0;
        });

        this.config.chunks.find({ns: dbName + "." + collName}).forEach(function(z) {
            if (x[z.shard])
                x[z.shard]++;
            else
                x[z.shard] = 1;
        });

        return x;
    };

    this.chunkDiff = function(collName, dbName) {
        var c = this.chunkCounts(collName, dbName);

        var min = 100000000;
        var max = 0;
        for (var s in c) {
            if (c[s] < min)
                min = c[s];
            if (c[s] > max)
                max = c[s];
        }

        print("ShardingTest input: " + tojson(c) + " min: " + min + " max: " + max);
        return max - min;
    };

    /**
     * Waits up to the specified timeout (with a default of 60s) for the balancer to execute one
     * round. If no round has been executed, throws an error.
     *
     * The mongosConnection parameter is optional and allows callers to specify a connection
     * different than the first mongos instance in the list.
     */
    this.awaitBalancerRound = function(timeoutMs, mongosConnection) {
        timeoutMs = timeoutMs || 60000;
        mongosConnection = mongosConnection || self.s0;

        // Get the balancer section from the server status of the config server primary
        function getBalancerStatus() {
            var balancerStatus =
                assert.commandWorked(mongosConnection.adminCommand({balancerStatus: 1}));
            if (balancerStatus.mode !== 'full') {
                throw Error('Balancer is not enabled');
            }

            return balancerStatus;
        }

        var initialStatus = getBalancerStatus();
        var currentStatus;
        assert.soon(function() {
            currentStatus = getBalancerStatus();
            return (currentStatus.numBalancerRounds - initialStatus.numBalancerRounds) != 0;
        }, 'Latest balancer status' + currentStatus, timeoutMs);
    };

    /**
     * Waits up to one minute for the difference in chunks between the most loaded shard and
     * least loaded shard to be 0 or 1, indicating that the collection is well balanced. This should
     * only be called after creating a big enough chunk difference to trigger balancing.
     */
    this.awaitBalance = function(collName, dbName, timeToWait) {
        timeToWait = timeToWait || 60000;

        assert.soon(function() {
            var x = self.chunkDiff(collName, dbName);
            print("chunk diff: " + x);
            return x < 2;
        }, "no balance happened", timeToWait);
    };

    this.getShard = function(coll, query, includeEmpty) {
        var shards = this.getShardsForQuery(coll, query, includeEmpty);
        assert.eq(shards.length, 1);
        return shards[0];
    };

    /**
     * Returns the shards on which documents matching a particular query reside.
     */
    this.getShardsForQuery = function(coll, query, includeEmpty) {
        if (!coll.getDB) {
            coll = this.s.getCollection(coll);
        }

        var explain = coll.find(query).explain("executionStats");
        var shards = [];

        var execStages = explain.executionStats.executionStages;
        var plannerShards = explain.queryPlanner.winningPlan.shards;

        if (execStages.shards) {
            for (var i = 0; i < execStages.shards.length; i++) {
                var hasResults = execStages.shards[i].executionStages.nReturned &&
                    execStages.shards[i].executionStages.nReturned > 0;
                if (includeEmpty || hasResults) {
                    shards.push(plannerShards[i].connectionString);
                }
            }
        }

        for (var i = 0; i < shards.length; i++) {
            for (var j = 0; j < this._connections.length; j++) {
                if (connectionURLTheSame(this._connections[j], shards[i])) {
                    shards[i] = this._connections[j];
                    break;
                }
            }
        }

        return shards;
    };

    this.shardColl = function(collName, key, split, move, dbName, waitForDelete) {
        split = (split != false ? (split || key) : split);
        move = (split != false && move != false ? (move || split) : false);

        if (collName.getDB)
            dbName = "" + collName.getDB();
        else
            dbName = dbName || "test";

        var c = dbName + "." + collName;
        if (collName.getDB) {
            c = "" + collName;
        }

        var isEmpty = (this.s.getCollection(c).count() == 0);

        if (!_isSharded(dbName)) {
            this.s.adminCommand({enableSharding: dbName});
        }

        var result = this.s.adminCommand({shardcollection: c, key: key});
        if (!result.ok) {
            printjson(result);
            assert(false);
        }

        if (split == false) {
            return;
        }

        result = this.s.adminCommand({split: c, middle: split});
        if (!result.ok) {
            printjson(result);
            assert(false);
        }

        if (move == false) {
            return;
        }

        var result;
        for (var i = 0; i < 5; i++) {
            var otherShard = this.getOther(this.getPrimaryShard(dbName)).name;
            result = this.s.adminCommand(
                {movechunk: c, find: move, to: otherShard, _waitForDelete: waitForDelete});
            if (result.ok)
                break;

            sleep(5 * 1000);
        }

        printjson(result);
        assert(result.ok);
    };

    /**
     * Kills the mongos with index n.
     */
    this.stopMongos = function(n, opts) {
        if (otherParams.useBridge) {
            MongoRunner.stopMongos(unbridgedMongos[n], undefined, opts);
            this["s" + n].stop();
        } else {
            MongoRunner.stopMongos(this["s" + n], undefined, opts);
        }
    };

    /**
     * Kills the shard mongod with index n.
     */
    this.stopMongod = function(n, opts) {
        if (otherParams.useBridge) {
            MongoRunner.stopMongod(unbridgedConnections[n], undefined, opts);
            this["d" + n].stop();
        } else {
            MongoRunner.stopMongod(this["d" + n], undefined, opts);
        }
    };

    /**
     * Kills the config server mongod with index n.
     */
    this.stopConfigServer = function(n, opts) {
        if (otherParams.useBridge) {
            MongoRunner.stopMongod(unbridgedConfigServers[n], undefined, opts);
            this._configServers[n].stop();
        } else {
            MongoRunner.stopMongod(this._configServers[n], undefined, opts);
        }
    };

    /**
     * Stops and restarts a mongos process.
     *
     * If opts is specified, the new mongos is started using those options. Otherwise, it is started
     * with its previous parameters.
     *
     * Warning: Overwrites the old s (if n = 0) admin, config, and sn member variables.
     */
    this.restartMongos = function(n, opts) {
        var mongos;

        if (otherParams.useBridge) {
            mongos = unbridgedMongos[n];
        } else {
            mongos = this["s" + n];
        }

        opts = opts || mongos;
        opts.port = opts.port || mongos.port;

        this.stopMongos(n);

        if (otherParams.useBridge) {
            var bridgeOptions =
                (opts !== mongos) ? opts.bridgeOptions : mongos.fullOptions.bridgeOptions;
            bridgeOptions = Object.merge(otherParams.bridgeOptions, bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: otherParams.useHostname ? hostName : "localhost",
                port: this._mongos[n].port,
                // The mongos processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + opts.port,
            });

            this._mongos[n] = new MongoBridge(bridgeOptions);
        }

        var newConn = MongoRunner.runMongos(opts);
        if (!newConn) {
            throw new Error("Failed to restart mongos " + n);
        }

        if (otherParams.useBridge) {
            this._mongos[n].connectToBridge();
            unbridgedMongos[n] = newConn;
        } else {
            this._mongos[n] = newConn;
        }

        this['s' + n] = this._mongos[n];
        if (n == 0) {
            this.s = this._mongos[n];
            this.admin = this._mongos[n].getDB('admin');
            this.config = this._mongos[n].getDB('config');
        }
    };

    /**
     * Stops and restarts a shard mongod process.
     *
     * If opts is specified, the new mongod is started using those options. Otherwise, it is started
     * with its previous parameters. The 'beforeRestartCallback' parameter is an optional function
     * that will be run after the MongoD is stopped, but before it is restarted. The intended uses
     * of the callback are modifications to the dbpath of the mongod that must be made while it is
     * stopped.
     *
     * Warning: Overwrites the old dn/shardn member variables.
     */
    this.restartMongod = function(n, opts, beforeRestartCallback) {
        var mongod;

        if (otherParams.useBridge) {
            mongod = unbridgedConnections[n];
        } else {
            mongod = this["d" + n];
        }

        opts = opts || mongod;
        opts.port = opts.port || mongod.port;

        this.stopMongod(n);

        if (otherParams.useBridge) {
            var bridgeOptions =
                (opts !== mongod) ? opts.bridgeOptions : mongod.fullOptions.bridgeOptions;
            bridgeOptions = Object.merge(otherParams.bridgeOptions, bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: otherParams.useHostname ? hostName : "localhost",
                port: this._connections[n].port,
                // The mongod processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + opts.port,
            });

            this._connections[n] = new MongoBridge(bridgeOptions);
        }

        if (arguments.length >= 3) {
            if (typeof(beforeRestartCallback) !== "function") {
                throw new Error("beforeRestartCallback must be a function but was of type " +
                                typeof(beforeRestartCallback));
            }
            beforeRestartCallback();
        }

        opts.restart = true;

        var newConn = MongoRunner.runMongod(opts);
        if (!newConn) {
            throw new Error("Failed to restart shard " + n);
        }

        if (otherParams.useBridge) {
            this._connections[n].connectToBridge();
            unbridgedConnections[n] = newConn;
        } else {
            this._connections[n] = newConn;
        }

        this["shard" + n] = this._connections[n];
        this["d" + n] = this._connections[n];
    };

    /**
     * Stops and restarts a config server mongod process.
     *
     * If opts is specified, the new mongod is started using those options. Otherwise, it is started
     * with its previous parameters.
     *
     * Warning: Overwrites the old cn/confign member variables.
     */
    this.restartConfigServer = function(n) {
        var mongod;

        if (otherParams.useBridge) {
            mongod = unbridgedConfigServers[n];
        } else {
            mongod = this["c" + n];
        }

        this.stopConfigServer(n);

        if (otherParams.useBridge) {
            var bridgeOptions =
                Object.merge(otherParams.bridgeOptions, mongod.fullOptions.bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: otherParams.useHostname ? hostName : "localhost",
                port: this._configServers[n].port,
                // The mongod processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + mongod.port,
            });

            this._configServers[n] = new MongoBridge(bridgeOptions);
        }

        mongod.restart = true;
        var newConn = MongoRunner.runMongod(mongod);
        if (!newConn) {
            throw new Error("Failed to restart config server " + n);
        }

        if (otherParams.useBridge) {
            this._configServers[n].connectToBridge();
            unbridgedConfigServers[n] = newConn;
        } else {
            this._configServers[n] = newConn;
        }

        this["config" + n] = this._configServers[n];
        this["c" + n] = this._configServers[n];
    };

    /**
     * Helper method for setting primary shard of a database and making sure that it was successful.
     * Note: first mongos needs to be up.
     */
    this.ensurePrimaryShard = function(dbName, shardName) {
        var db = this.s0.getDB('admin');
        var res = db.adminCommand({movePrimary: dbName, to: shardName});
        assert(res.ok || res.errmsg == "it is already the primary", tojson(res));
    };

    // ShardingTest initialization

    assert(isObject(params), 'ShardingTest configuration must be a JSON object');

    var testName = params.name || "test";
    var otherParams = Object.merge(params, params.other || {});

    var numShards = otherParams.hasOwnProperty('shards') ? otherParams.shards : 2;
    var verboseLevel = otherParams.hasOwnProperty('verbose') ? otherParams.verbose : 1;
    var numMongos = otherParams.hasOwnProperty('mongos') ? otherParams.mongos : 1;
    var numConfigs = otherParams.hasOwnProperty('config') ? otherParams.config : 3;
    var waitForCSRSSecondaries = otherParams.hasOwnProperty('waitForCSRSSecondaries')
        ? otherParams.waitForCSRSSecondaries
        : true;

    // Allow specifying mixed-type options like this:
    // { mongos : [ { noprealloc : "" } ],
    //   config : [ { smallfiles : "" } ],
    //   shards : { rs : true, d : true } }
    if (Array.isArray(numShards)) {
        for (var i = 0; i < numShards.length; i++) {
            otherParams["d" + i] = numShards[i];
        }

        numShards = numShards.length;
    } else if (isObject(numShards)) {
        var tempCount = 0;
        for (var i in numShards) {
            otherParams[i] = numShards[i];
            tempCount++;
        }

        numShards = tempCount;
    }

    if (Array.isArray(numMongos)) {
        for (var i = 0; i < numMongos.length; i++) {
            otherParams["s" + i] = numMongos[i];
        }

        numMongos = numMongos.length;
    } else if (isObject(numMongos)) {
        var tempCount = 0;
        for (var i in numMongos) {
            otherParams[i] = numMongos[i];
            tempCount++;
        }

        numMongos = tempCount;
    }

    if (Array.isArray(numConfigs)) {
        for (var i = 0; i < numConfigs.length; i++) {
            otherParams["c" + i] = numConfigs[i];
        }

        numConfigs = numConfigs.length;
    } else if (isObject(numConfigs)) {
        var tempCount = 0;
        for (var i in numConfigs) {
            otherParams[i] = numConfigs[i];
            tempCount++;
        }

        numConfigs = tempCount;
    }

    otherParams.useHostname = otherParams.useHostname == undefined ? true : otherParams.useHostname;
    otherParams.useBridge = otherParams.useBridge || false;
    otherParams.bridgeOptions = otherParams.bridgeOptions || {};

    var keyFile = otherParams.keyFile;
    var hostName = getHostName();

    this._testName = testName;
    this._otherParams = otherParams;

    var pathOpts = {testName: testName};

    for (var k in otherParams) {
        if (k.startsWith("rs") && otherParams[k] != undefined) {
            break;
        }
    }

    this._connections = [];
    this._rs = [];
    this._rsObjects = [];

    if (otherParams.useBridge) {
        var unbridgedConnections = [];
        var unbridgedConfigServers = [];
        var unbridgedMongos = [];
    }

    // Start the MongoD servers (shards)
    for (var i = 0; i < numShards; i++) {
        if (otherParams.rs || otherParams["rs" + i]) {
            var setName = testName + "-rs" + i;

            var rsDefaults = {
                useHostname: otherParams.useHostname,
                noJournalPrealloc: otherParams.nopreallocj,
                oplogSize: 16,
                shardsvr: '',
                pathOpts: Object.merge(pathOpts, {shard: i})
            };

            rsDefaults = Object.merge(rsDefaults, otherParams.rs);
            rsDefaults = Object.merge(rsDefaults, otherParams.rsOptions);
            rsDefaults = Object.merge(rsDefaults, otherParams["rs" + i]);
            rsDefaults.nodes = rsDefaults.nodes || otherParams.numReplicas;
            var rsSettings = rsDefaults.settings;
            delete rsDefaults.settings;

            var numReplicas = rsDefaults.nodes || 3;
            delete rsDefaults.nodes;

            var protocolVersion = rsDefaults.protocolVersion;
            delete rsDefaults.protocolVersion;
            var initiateTimeout = rsDefaults.initiateTimeout;
            delete rsDefaults.initiateTimeout;

            var rs = new ReplSetTest({
                name: setName,
                nodes: numReplicas,
                useHostName: otherParams.useHostname,
                useBridge: otherParams.useBridge,
                bridgeOptions: otherParams.bridgeOptions,
                keyFile: keyFile,
                protocolVersion: protocolVersion,
                settings: rsSettings
            });

            this._rs[i] =
                {setName: setName, test: rs, nodes: rs.startSet(rsDefaults), url: rs.getURL()};

            rs.initiate(null, null, initiateTimeout);

            this["rs" + i] = rs;
            this._rsObjects[i] = rs;

            _alldbpaths.push(null);
            this._connections.push(null);

            if (otherParams.useBridge) {
                unbridgedConnections.push(null);
            }
        } else {
            var options = {
                useHostname: otherParams.useHostname,
                noJournalPrealloc: otherParams.nopreallocj,
                pathOpts: Object.merge(pathOpts, {shard: i}),
                dbpath: "$testName$shard",
                shardsvr: '',
                keyFile: keyFile
            };

            if (otherParams.shardOptions && otherParams.shardOptions.binVersion) {
                otherParams.shardOptions.binVersion =
                    MongoRunner.versionIterator(otherParams.shardOptions.binVersion);
            }

            options = Object.merge(options, otherParams.shardOptions);
            options = Object.merge(options, otherParams["d" + i]);

            options.port = options.port || allocatePort();

            if (otherParams.useBridge) {
                var bridgeOptions =
                    Object.merge(otherParams.bridgeOptions, options.bridgeOptions || {});
                bridgeOptions = Object.merge(bridgeOptions, {
                    hostName: otherParams.useHostname ? hostName : "localhost",
                    // The mongod processes identify themselves to mongobridge as host:port, where
                    // the host is the actual hostname of the machine and not localhost.
                    dest: hostName + ":" + options.port,
                });

                var bridge = new MongoBridge(bridgeOptions);
            }

            var conn = MongoRunner.runMongod(options);
            if (!conn) {
                throw new Error("Failed to start shard " + i);
            }

            if (otherParams.useBridge) {
                bridge.connectToBridge();
                this._connections.push(bridge);
                unbridgedConnections.push(conn);
            } else {
                this._connections.push(conn);
            }

            _alldbpaths.push(testName + i);
            this["shard" + i] = this._connections[i];
            this["d" + i] = this._connections[i];

            this._rs[i] = null;
            this._rsObjects[i] = null;
        }
    }

    // Do replication on replica sets if required
    for (var i = 0; i < numShards; i++) {
        if (!otherParams.rs && !otherParams["rs" + i]) {
            continue;
        }

        var rs = this._rs[i].test;
        rs.getPrimary().getDB("admin").foo.save({x: 1});

        if (keyFile) {
            authutil.asCluster(rs.nodes, keyFile, function() {
                rs.awaitReplication();
            });
        }

        rs.awaitSecondaryNodes();

        var rsConn = new Mongo(rs.getURL());
        rsConn.name = rs.getURL();

        this._connections[i] = rsConn;
        this["shard" + i] = rsConn;
        rsConn.rs = rs;
    }

    this._configServers = [];

    // Using replica set for config servers
    var rstOptions = {
        useHostName: otherParams.useHostname,
        useBridge: otherParams.useBridge,
        bridgeOptions: otherParams.bridgeOptions,
        keyFile: keyFile,
        name: testName + "-configRS",
    };

    // when using CSRS, always use wiredTiger as the storage engine
    var startOptions = {
        pathOpts: pathOpts,
        // Ensure that journaling is always enabled for config servers.
        journal: "",
        configsvr: "",
        noJournalPrealloc: otherParams.nopreallocj,
        storageEngine: "wiredTiger",
    };

    if (otherParams.configOptions && otherParams.configOptions.binVersion) {
        otherParams.configOptions.binVersion =
            MongoRunner.versionIterator(otherParams.configOptions.binVersion);
    }

    startOptions = Object.merge(startOptions, otherParams.configOptions);
    rstOptions = Object.merge(rstOptions, otherParams.configReplSetTestOptions);

    var nodeOptions = [];
    for (var i = 0; i < numConfigs; ++i) {
        nodeOptions.push(otherParams["c" + i] || {});
    }

    rstOptions.nodes = nodeOptions;

    // Start the config server's replica set
    this.configRS = new ReplSetTest(rstOptions);
    this.configRS.startSet(startOptions);

    var config = this.configRS.getReplSetConfig();
    config.configsvr = true;
    config.settings = config.settings || {};
    var initiateTimeout = otherParams.rsOptions && otherParams.rsOptions.initiateTimeout;
    this.configRS.initiate(config, null, initiateTimeout);

    // Wait for master to be elected before starting mongos
    var csrsPrimary = this.configRS.getPrimary();

    // If chunkSize has been requested for this test, write the configuration
    if (otherParams.chunkSize) {
        function setChunkSize() {
            assert.writeOK(csrsPrimary.getDB('config').settings.update(
                {_id: 'chunksize'},
                {$set: {value: otherParams.chunkSize}},
                {upsert: true, writeConcern: {w: 'majority', wtimeout: 30000}}));
        }

        if (keyFile) {
            authutil.asCluster(csrsPrimary, keyFile, setChunkSize);
        } else {
            setChunkSize();
        }
    }

    this._configDB = this.configRS.getURL();
    this._configServers = this.configRS.nodes;
    for (var i = 0; i < numConfigs; ++i) {
        var conn = this._configServers[i];
        this["config" + i] = conn;
        this["c" + i] = conn;
    }

    printjson('Config servers: ' + this._configDB);

    var configConnection = _connectWithRetry(this._configDB);

    print("ShardingTest " + this._testName + " :\n" +
          tojson({config: this._configDB, shards: this._connections}));

    this._mongos = [];

    // Start the MongoS servers
    for (var i = 0; i < numMongos; i++) {
        options = {
            useHostname: otherParams.useHostname,
            pathOpts: Object.merge(pathOpts, {mongos: i}),
            configdb: this._configDB,
            verbose: verboseLevel || 0,
            keyFile: keyFile,
        };

        if (otherParams.mongosOptions && otherParams.mongosOptions.binVersion) {
            otherParams.mongosOptions.binVersion =
                MongoRunner.versionIterator(otherParams.mongosOptions.binVersion);
        }

        options = Object.merge(options, otherParams.mongosOptions);
        options = Object.merge(options, otherParams["s" + i]);

        options.port = options.port || allocatePort();

        if (otherParams.useBridge) {
            var bridgeOptions =
                Object.merge(otherParams.bridgeOptions, options.bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: otherParams.useHostname ? hostName : "localhost",
                // The mongos processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + options.port,
            });

            var bridge = new MongoBridge(bridgeOptions);
        }

        var conn = MongoRunner.runMongos(options);
        if (!conn) {
            throw new Error("Failed to start mongos " + i);
        }

        if (otherParams.useBridge) {
            bridge.connectToBridge();
            this._mongos.push(bridge);
            unbridgedMongos.push(conn);
        } else {
            this._mongos.push(conn);
        }

        if (i === 0) {
            this.s = this._mongos[i];
            this.admin = this._mongos[i].getDB('admin');
            this.config = this._mongos[i].getDB('config');
        }

        this["s" + i] = this._mongos[i];
    }

    _extendWithShMethods();

    // If auth is enabled for the test, login the mongos connections as system in order to configure
    // the instances and then log them out again.
    if (keyFile) {
        authutil.asCluster(this._mongos, keyFile, _configureCluster);
    } else {
        _configureCluster();
    }

    try {
        if (!otherParams.manualAddShard) {
            this._shardNames = [];

            var testName = this._testName;
            var admin = this.admin;
            var shardNames = this._shardNames;

            this._connections.forEach(function(z) {
                var n = z.name || z.host || z;

                print("ShardingTest " + testName + " going to add shard : " + n);

                var result = admin.runCommand({addshard: n});
                assert.commandWorked(result, "Failed to add shard " + n);

                shardNames.push(result.shardAdded);
                z.shardName = result.shardAdded;
            });
        }
    } catch (e) {
        // Clean up the running procceses on failure
        print("Failed to add shards, stopping cluster.");
        this.stop();
        throw e;
    }

    if (waitForCSRSSecondaries) {
        // Ensure that all CSRS nodes are up to date. This is strictly needed for tests that use
        // multiple mongoses. In those cases, the first mongos initializes the contents of the
        // 'config' database, but without waiting for those writes to replicate to all the
        // config servers then the secondary mongoses risk reading from a stale config server
        // and seeing an empty config database.
        this.configRS.awaitLastOpCommitted();
    }

    if (jsTestOptions().keyFile) {
        jsTest.authenticate(configConnection);
        jsTest.authenticateNodes(this._configServers);
        jsTest.authenticateNodes(this._mongos);
    }
};
