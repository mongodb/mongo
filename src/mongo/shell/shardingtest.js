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
 *         For other options, @see ReplSetTest#initiate
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
 *       enableAutoSplit {boolean} : if true, enable autosplitting; else, default to the
 *          enableBalancer setting
 *       manualAddShard {boolean}: shards will not be added if true.
 *
 *       migrationLockAcquisitionMaxWaitMS {number}: number of milliseconds to acquire the migration
 *          lock.
 *
 *       useBridge {boolean}: If true, then a mongobridge process is started for each node in the
 *          sharded cluster. Defaults to false.
 *
 *       causallyConsistent {boolean}: Specifies whether the connections to the replica set nodes
 *          should be created with the 'causal consistency' flag enabled, which means they will
 *          gossip the cluster time and add readConcern afterClusterTime where applicable.
 *          Defaults to false.
 *
 *       bridgeOptions {Object}: Options to apply to all mongobridge processes. Defaults to {}.
 *
 *       // replica Set only:
 *       rsOptions {Object}: same as the rs property above. Can be used to
 *         specify options that are common all replica members.
 *       useHostname {boolean}: if true, use hostname of machine,
 *         otherwise use localhost
 *       numReplicas {number}
 *
 *     }
 *   }
 *
 * Member variables:
 * s {Mongo} - connection to the first mongos
 * s0, s1, ... {Mongo} - connection to different mongos
 * rs0, rs1, ... {ReplSetTest} - test objects to replica sets
 * shard0, shard1, ... {Mongo} - connection to shards
 * config0, config1, ... {Mongo} - connection to config servers
 * c0, c1, ... {Mongo} - same as config0, config1, ...
 * configRS - the config ReplSetTest object
 */
var ShardingTest = function(params) {
    if (!(this instanceof ShardingTest)) {
        return new ShardingTest(params);
    }

    // Capture the 'this' reference
    var self = this;

    // Used for counting the test duration
    var _startTime = new Date();

    // Timeout to be used for operations scheduled by the sharding test, which must wait for write
    // concern (5 minutes)
    var kDefaultWTimeoutMs = 5 * 60 * 1000;

    // Ensure we don't mutate the passed-in parameters.
    params = Object.extend({}, params, true);

    // Publicly exposed variables

    /**
     * Tries to load the 'jstests/libs/parallelTester.js' dependency. Returns true if the file is
     * loaded successfully, and false otherwise.
     */
    function tryLoadParallelTester() {
        try {
            load("jstests/libs/parallelTester.js");  // For Thread.
            return true;
        } catch (e) {
            return false;
        }
    }

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
                    return sh[fn].apply(sh, arguments);
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
        if (!otherParams.enableBalancer) {
            self.stopBalancer();
        }

        if (!otherParams.enableAutoSplit) {
            self.disableAutoSplit();
        } else if (!otherParams.enableBalancer) {
            // Turn on autoSplit since disabling balancer also turns auto split off.
            self.enableAutoSplit();
        }
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
        throw Error("couldn't find dbname: " + dbname +
                    " in config.databases. Total DBs: " + countDBsFound);
    };

    this.getNonPrimaries = function(dbname) {
        var x = this.config.databases.findOne({_id: dbname});
        if (!x) {
            this.config.databases.find().forEach(printjson);
            throw Error("couldn't find dbname: " + dbname +
                        " total: " + this.config.databases.count());
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

        throw Error("can't find server connection for db '" + dbname +
                    "'s primary shard: " + tojson(primaryShard));
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

    this.stopAllConfigServers = function(opts, forRestart = undefined) {
        this.configRS.stopSet(undefined, forRestart, opts);
    };

    this.stopAllShards = function(opts, forRestart = undefined) {
        this._rs.forEach((rs) => {
            rs.test.stopSet(15, forRestart, opts);
        });
    };

    this.stopAllMongos = function(opts) {
        for (var i = 0; i < this._mongos.length; i++) {
            this.stopMongos(i, opts);
        }
    };

    this.stop = function(opts = {}) {
        this.checkUUIDsConsistentAcrossCluster();
        this.checkIndexesConsistentAcrossCluster();
        this.checkOrphansAreDeleted();
        this.checkRoutingTableConsistency();

        if (jsTestOptions().alwaysUseLogFiles) {
            if (opts.noCleanData === false) {
                throw new Error("Always using log files, but received conflicting option.");
            }

            opts.noCleanData = true;
        }

        this.stopAllMongos(opts);

        let startTime = new Date();  // Measure the execution time of shutting down shards.
        this.stopAllShards(opts);
        print("ShardingTest stopped all shards, took " + (new Date() - startTime) + "ms for " +
              this._connections.length + " shards.");

        this.stopAllConfigServers(opts);

        var timeMillis = new Date().getTime() - _startTime.getTime();

        print('*** ShardingTest ' + this._testName + " completed successfully in " +
              (timeMillis / 1000) + " seconds ***");
    };

    this.adminCommand = function(cmd) {
        var res = this.admin.runCommand(cmd);
        if (res && res.ok == 1)
            return true;

        throw _getErrorWithCode(res, "command " + tojson(cmd) + " failed: " + tojson(res));
    };

    this.restartAllConfigServers = function(opts) {
        this.configRS.startSet(opts, true);
    };

    this.restartAllShards = function(opts) {
        this._rs.forEach((rs) => {
            rs.test.startSet(opts, true);
        });
    };

    this.restartAllMongos = function(opts) {
        for (var i = 0; i < this._mongos.length; i++) {
            this.restartMongos(i, opts);
        }
    };

    this.forEachConnection = function(fn) {
        this._connections.forEach(function(conn) {
            fn(conn);
        });
    };

    this.forEachMongos = function(fn) {
        this._mongos.forEach(function(conn) {
            fn(conn);
        });
    };

    this.forEachConfigServer = function(fn) {
        this.configRS.nodes.forEach(function(conn) {
            fn(conn);
        });
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
        if (ns) {
            let query = {};
            let sorting_criteria = {};
            const collection = this.config.collections.findOne({_id: ns});
            if (!collection) {
                return "";
            }

            if (collection.timestamp) {
                const collectionUUID = collection.uuid;
                assert.neq(collectionUUID, null);

                query.uuid = collectionUUID;
                sorting_criteria = {uuid: 1, min: 1};
            } else {
                query.ns = ns;
                sorting_criteria = {ns: 1, min: 1};
            }

            let s = "";
            this.config.chunks.find(query).sort(sorting_criteria).forEach(function(z) {
                s += " \t" + z._id + "\t" + z.lastmod.t + "|" + z.lastmod.i + "\t" + tojson(z.min) +
                    " -> " + tojson(z.max) + " " + z.shard + " " + ns + "\n";
            });

            return s;
        } else {
            // call get chunks String for every namespace in the collections
            let collections_cursor = this.config.collections.find();
            let s = "";
            while (collections_cursor.hasNext()) {
                var ns = collections_cursor.next()._id;
                s += this.getChunksString(ns);
            }
            return s;
        }
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

    /**
     * Returns the number of shards which contain the given dbName.collName collection
     */
    this.onNumShards = function(dbName, collName) {
        return this.shardCounts(dbName, collName)
            .reduce((total, currentValue) => total + (currentValue > 0 ? 1 : 0), 0);
    };

    /**
     * Returns an array of the size of numShards where each element is the number of documents on
     * that particular shard
     */
    this.shardCounts = function(dbName, collName) {
        return this._connections.map((connection) =>
                                         connection.getDB(dbName).getCollection(collName).count());
    };

    this.chunkCounts = function(collName, dbName) {
        dbName = dbName || "test";

        var x = {};
        this.config.shards.find().forEach(function(z) {
            x[z._id] = 0;
        });

        var coll = this.config.collections.findOne({_id: dbName + "." + collName});
        var chunksQuery = (function() {
            if (coll.timestamp != null) {
                return {uuid: coll.uuid};
            } else {
                return {ns: dbName + "." + collName};
            }
        }());

        this.config.chunks.find(chunksQuery).forEach(function(z) {
            if (x[z.shard])
                x[z.shard]++;
            else
                x[z.shard] = 1;
        });

        return x;
    };

    this.chunkDiff = function(collName, dbName) {
        var c = this.chunkCounts(collName, dbName);

        var min = Number.MAX_VALUE;
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
     * Waits up to one minute for the difference in chunks between the most loaded shard and
     * least loaded shard to be 0 or 1, indicating that the collection is well balanced. This should
     * only be called after creating a big enough chunk difference to trigger balancing.
     */
    this.awaitBalance = function(collName, dbName, timeToWait, interval) {
        timeToWait = timeToWait || 60000;
        interval = interval || 200;

        const mongos = this.s;
        assert.soon(function() {
            return assert
                .commandWorked(
                    mongos.adminCommand({balancerCollectionStatus: dbName + '.' + collName}))
                .balancerCompliant;
        }, 'Timed out waiting for the collection to be balanced', timeToWait, interval);
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

        assert.commandWorked(this.s.adminCommand({enableSharding: dbName}));

        var result = assert.commandWorked(this.s.adminCommand({shardcollection: c, key: key}));

        if (split == false) {
            return;
        }

        result = assert.commandWorked(this.s.adminCommand({split: c, middle: split}));

        if (move == false) {
            return;
        }

        var result;
        for (var i = 0; i < 5; i++) {
            var otherShard = this.getOther(this.getPrimaryShard(dbName)).name;
            let cmd = {movechunk: c, find: move, to: otherShard};

            if (waitForDelete != null) {
                cmd._waitForDelete = waitForDelete;
            }

            result = this.s.adminCommand(cmd);
            if (result.ok)
                break;

            sleep(5 * 1000);
        }

        assert.commandWorked(result);
    };

    /**
     * Wait for sharding to be initialized.
     */
    this.waitForShardingInitialized = function(timeoutMs = 60 * 1000) {
        const getShardVersion = (client, timeout) => {
            assert.soon(() => {
                // The choice of namespace (local.fooCollection) does not affect the output.
                var res = client.adminCommand({getShardVersion: "local.fooCollection"});
                return res.ok == 1;
            }, "timeout waiting for sharding to be initialized on mongod", timeout, 0.1);
        };

        var start = new Date();

        for (var i = 0; i < this._rs.length; ++i) {
            var replSet = this._rs[i];
            if (!replSet)
                continue;
            nodes = replSet.test.nodes;
            keyFileUsed = replSet.test.keyFile;

            for (var j = 0; j < nodes.length; ++j) {
                diff = (new Date()).getTime() - start.getTime();
                var currNode = nodes[j];
                // Skip arbiters
                if (currNode.getDB('admin')._helloOrLegacyHello().arbiterOnly) {
                    continue;
                }

                const tlsOptions = ['preferTLS', 'requireTLS'];
                const sslOptions = ['preferSSL', 'requireSSL'];
                const TLSEnabled = currNode.fullOptions &&
                    (tlsOptions.includes(currNode.fullOptions.tlsMode) ||
                     sslOptions.includes(currNode.fullOptions.sslMode));

                const x509AuthRequired = (conn.fullOptions && conn.fullOptions.clusterAuthMode &&
                                          conn.fullOptions.clusterAuthMode === "x509");

                if (keyFileUsed) {
                    authutil.asCluster(currNode, keyFileUsed, () => {
                        getShardVersion(currNode, timeoutMs - diff);
                    });
                } else if (x509AuthRequired && TLSEnabled) {
                    const exitCode = _runMongoProgram(
                        ...["mongo",
                            currNode.host,
                            "--tls",
                            "--tlsAllowInvalidHostnames",
                            "--tlsCertificateKeyFile",
                            currNode.fullOptions.tlsCertificateKeyFile
                                ? currNode.fullOptions.tlsCertificateKeyFile
                                : currNode.fullOptions.sslPEMKeyFile,
                            "--tlsCAFile",
                            currNode.fullOptions.tlsCAFile ? currNode.fullOptions.tlsCAFile
                                                           : currNode.fullOptions.sslCAFile,
                            "--authenticationDatabase=$external",
                            "--authenticationMechanism=MONGODB-X509",
                            "--eval",
                            `(${getShardVersion.toString()})(db.getMongo(), ` +
                                (timeoutMs - diff).toString() + `)`,
                    ]);
                    assert.eq(0, exitCode, "parallel shell for x509 auth failed");
                } else {
                    getShardVersion(currNode, timeoutMs - diff);
                }
            }
        }
    };

    /**
     * Kills the mongos with index n.
     *
     * @param {boolean} [extraOptions.waitPid=true] if true, we will wait for the process to
     * terminate after stopping it.
     */
    this.stopMongos = function(n, opts, {
        waitpid: waitpid = true,
    } = {}) {
        if (otherParams.useBridge) {
            MongoRunner.stopMongos(unbridgedMongos[n], undefined, opts, waitpid);
            this["s" + n].stop();
        } else {
            MongoRunner.stopMongos(this["s" + n], undefined, opts, waitpid);
        }
    };

    /**
     * Kills the config server mongod with index n.
     */
    this.stopConfigServer = function(n, opts) {
        this.configRS.stop(n, undefined, opts);
    };

    /**
     * Stops and restarts a mongos process.
     *
     * If 'opts' is not specified, starts the mongos with its previous parameters.  If 'opts' is
     * specified and 'opts.restart' is false or missing, starts mongos with the parameters specified
     * in 'opts'.  If opts is specified and 'opts.restart' is true, merges the previous options
     * with the options specified in 'opts', with the options in 'opts' taking precedence.
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

        if (opts.restart) {
            opts = Object.merge(mongos.fullOptions, opts);

            // If the mongos is being restarted with a newer version, make sure we remove any
            // options that no longer exist in the newer version.
            if (MongoRunner.areBinVersionsTheSame('latest', opts.binVersion)) {
                delete opts.noAutoSplit;
            }
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
     * Restarts each node in a particular shard replica set using the shard's original startup
     * options by default.
     *
     * Option { startClean : true } forces clearing the data directory.
     * Option { auth : Object } object that contains the auth details for admin credentials.
     *   Should contain the fields 'user' and 'pwd'
     *
     *
     * @param {int} shard server number (0, 1, 2, ...) to be restarted
     */
    this.restartShardRS = function(n, options, signal, wait) {
        for (let i = 0; i < this["rs" + n].nodeList().length; i++) {
            this["rs" + n].restart(i);
        }

        this["rs" + n].awaitSecondaryNodes();
        this._connections[n] = new Mongo(this["rs" + n].getURL());
        this["shard" + n] = this._connections[n];
    };

    /**
     * Stops and restarts a config server mongod process.
     *
     * If opts is specified, the new mongod is started using those options. Otherwise, it is
     * started
     * with its previous parameters.
     *
     * Warning: Overwrites the old cn/confign member variables.
     */
    this.restartConfigServer = function(n, options, signal, wait) {
        this.configRS.restart(n, options, signal, wait);
        this["config" + n] = this.configRS.nodes[n];
        this["c" + n] = this.configRS.nodes[n];
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

    /**
     * Returns a document {isMixedVersion: <bool>, oldestBinVersion: <string>}.
     * The 'isMixedVersion' field is true if any settings to ShardingTest or jsTestOptions indicate
     * this is a multiversion cluster.
     * The 'oldestBinVersion' field is set to the oldest binary version used in this cluster, one of
     * 'latest', 'last-continuous' and 'last-lts'.
     * Note: Mixed version cluster with binary versions older than 'last-lts' is not supported. If
     * such binary exists in the cluster, this function assumes this is not a mixed version cluster
     * and returns 'oldestBinVersion' as 'latest'.
     *
     * Checks for bin versions via:
     *     jsTestOptions().shardMixedBinVersions, jsTestOptions().mongosBinVersion,
     *     otherParams.configOptions.binVersion, otherParams.shardOptions.binVersion,
     *     otherParams.mongosOptions.binVersion
     */
    this.getClusterVersionInfo = function() {
        function clusterHasBinVersion(version) {
            const binVersion = MongoRunner.getBinVersionFor(version);
            const hasBinVersionInParams = (params) => {
                return params && params.binVersion &&
                    MongoRunner.areBinVersionsTheSame(
                        binVersion, MongoRunner.getBinVersionFor(params.binVersion));
            };

            // Must check shardMixedBinVersion because it causes shardOptions.binVersion to be an
            // object (versionIterator) rather than a version string. Must check mongosBinVersion,
            // as well, because it does not update mongosOptions.binVersion.
            const isMixedVersionShard = jsTestOptions().shardMixedBinVersions &&
                MongoRunner.areBinVersionsTheSame(binVersion,
                                                  jsTestOptions().shardMixedBinVersions);
            const isMixedVersionMongos = jsTestOptions().mongosBinVersion &&
                MongoRunner.areBinVersionsTheSame(binVersion, jsTestOptions().mongosBinVersion);
            if (isMixedVersionShard || isMixedVersionMongos) {
                return true;
            }

            // Check for config servers.
            if (hasBinVersionInParams(otherParams.configOptions)) {
                return true;
            }
            for (let i = 0; i < numConfigs; ++i) {
                if (hasBinVersionInParams(otherParams['c' + i])) {
                    return true;
                }
            }

            // Check for mongod servers.
            if (hasBinVersionInParams(otherParams.shardOptions)) {
                return true;
            }
            if (hasBinVersionInParams(otherParams.rs)) {
                return true;
            }
            for (let i = 0; i < numShards; ++i) {
                if (hasBinVersionInParams(otherParams['d' + i])) {
                    return true;
                }
                if (hasBinVersionInParams(otherParams['rs' + i])) {
                    return true;
                }
            }

            // Check for mongos servers.
            if (hasBinVersionInParams(otherParams.mongosOptions)) {
                return true;
            }
            for (let i = 0; i < numMongos; ++i) {
                if (hasBinVersionInParams(otherParams['s' + i])) {
                    return true;
                }
            }

            return false;
        }

        let hasLastLTS = clusterHasBinVersion("last-lts");
        let hasLastContinuous = clusterHasBinVersion("last-continuous");
        if ((lastLTSFCV !== lastContinuousFCV) && hasLastLTS && hasLastContinuous) {
            throw new Error("Can only specify one of 'last-lts' and 'last-continuous' " +
                            "in binVersion, not both.");
        }
        if (hasLastLTS) {
            return {isMixedVersion: true, oldestBinVersion: "last-lts"};
        } else if (hasLastContinuous) {
            return {isMixedVersion: true, oldestBinVersion: "last-continuous"};
        } else {
            return {isMixedVersion: false, oldestBinVersion: "latest"};
        }
    };

    /**
     * Runs a find on the namespace to force a refresh of the node's catalog cache.
     */
    this.refreshCatalogCacheForNs = function(node, ns) {
        node.getCollection(ns).findOne();
    };

    /**
     * Returns if there is a new feature compatibility version for the "latest" version. This must
     * be manually changed if and when there is a new feature compatibility version.
     */
    function _hasNewFeatureCompatibilityVersion() {
        return true;
    }

    /**
     * Returns the total number of mongod nodes across all shards, excluding config server nodes.
     * Used only for diagnostic logging.
     */
    function totalNumShardNodes() {
        const numNodesPerReplSet = self._rs.map(r => r.test.nodes.length);
        return numNodesPerReplSet.reduce((a, b) => a + b, 0);
    }

    // ShardingTest initialization

    assert(isObject(params), 'ShardingTest configuration must be a JSON object');

    var testName = params.name || jsTest.name();
    var otherParams = Object.merge(params, params.other || {});

    var numShards = otherParams.hasOwnProperty('shards') ? otherParams.shards : 2;
    var mongosVerboseLevel = otherParams.hasOwnProperty('verbose') ? otherParams.verbose : 1;
    var numMongos = otherParams.hasOwnProperty('mongos') ? otherParams.mongos : 1;
    var numConfigs = otherParams.hasOwnProperty('config') ? otherParams.config : 3;

    if ("shardAsReplicaSet" in otherParams) {
        throw new Error("Use of deprecated option 'shardAsReplicaSet'");
    }

    // Default enableBalancer to false.
    otherParams.enableBalancer =
        ("enableBalancer" in otherParams) && (otherParams.enableBalancer === true);

    // Let autosplit behavior match that of the balancer if autosplit is not explicitly set.
    if (!("enableAutoSplit" in otherParams)) {
        otherParams.enableAutoSplit = otherParams.enableBalancer;
    }

    // Allow specifying mixed-type options like this:
    // { mongos : [ { bind_ip : "localhost" } ],
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
    otherParams.causallyConsistent = otherParams.causallyConsistent || false;

    if (jsTestOptions().networkMessageCompressors) {
        otherParams.bridgeOptions["networkMessageCompressors"] =
            jsTestOptions().networkMessageCompressors;
    }

    this.keyFile = otherParams.keyFile;
    var hostName = otherParams.host === undefined ? getHostName() : otherParams.host;

    this._testName = testName;
    this._otherParams = otherParams;

    var pathOpts = {testName: testName};

    this._connections = [];
    this._rs = [];
    this._rsObjects = [];

    let unbridgedMongos;
    let _makeAllocatePortFn;
    let _allocatePortForMongos;
    let _allocatePortForBridgeForMongos;
    let _allocatePortForShard;
    let _allocatePortForBridgeForShard;

    if (otherParams.useBridge) {
        unbridgedMongos = [];

        _makeAllocatePortFn = (preallocatedPorts, errorMessage) => {
            let idxNextNodePort = 0;

            return function() {
                if (idxNextNodePort >= preallocatedPorts.length) {
                    throw new Error(errorMessage(preallocatedPorts.length));
                }

                const nextPort = preallocatedPorts[idxNextNodePort];
                ++idxNextNodePort;
                return nextPort;
            };
        };

        let errorMessage = (length) =>
            "Cannot use more than " + length + " mongos processes when useBridge=true";
        _allocatePortForBridgeForMongos =
            _makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset), errorMessage);
        _allocatePortForMongos =
            _makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset), errorMessage);

        errorMessage = (length) => "Cannot use more than " + length + " shards when useBridge=true";
        _allocatePortForBridgeForShard =
            _makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset), errorMessage);
        _allocatePortForShard =
            _makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset), errorMessage);
    } else {
        _allocatePortForBridgeForShard = _allocatePortForBridgeForMongos = function() {
            throw new Error("Using mongobridge isn't enabled for this sharded cluster");
        };
        _allocatePortForShard = _allocatePortForMongos = allocatePort;
    }

    otherParams.migrationLockAcquisitionMaxWaitMS =
        otherParams.migrationLockAcquisitionMaxWaitMS || 30000;

    let randomSeedAlreadySet = false;

    if (jsTest.options().useRandomBinVersionsWithinReplicaSet) {
        // We avoid setting the random seed unequivocally to avoid unexpected behavior in tests
        // that already make use of Random.setRandomSeed(). This conditional can be removed if
        // it becomes the standard to always be generating the seed through ShardingTest.
        Random.setRandomSeed(jsTest.options().seed);
        randomSeedAlreadySet = true;
    }

    //
    // Start each shard replica set.
    //
    let startTime = new Date();  // Measure the execution time of startup and initiate.
    for (var i = 0; i < numShards; i++) {
        var setName = testName + "-rs" + i;

        var rsDefaults = {
            useHostname: otherParams.useHostname,
            oplogSize: 16,
            shardsvr: '',
            pathOpts: Object.merge(pathOpts, {shard: i}),
        };

        if (otherParams.rs || otherParams["rs" + i]) {
            if (otherParams.rs) {
                rsDefaults = Object.merge(rsDefaults, otherParams.rs);
            }
            if (otherParams["rs" + i]) {
                rsDefaults = Object.merge(rsDefaults, otherParams["rs" + i]);
            }
            rsDefaults = Object.merge(rsDefaults, otherParams.rsOptions);
            rsDefaults.nodes = rsDefaults.nodes || otherParams.numReplicas;
        } else {
            if (jsTestOptions().shardMixedBinVersions) {
                if (!otherParams.shardOptions) {
                    otherParams.shardOptions = {};
                }
                // If the test doesn't depend on specific shard binVersions, create a mixed
                // version
                // shard cluster that randomly assigns shard binVersions, half "latest" and half
                // "last-continuous" or "last-lts".
                // shardMixedBinVersions.
                if (!otherParams.shardOptions.binVersion) {
                    Random.setRandomSeed();
                    otherParams.shardOptions.binVersion = MongoRunner.versionIterator(
                        ["latest", jsTestOptions().shardMixedBinVersions], true);
                }
            }

            if (otherParams.shardOptions && otherParams.shardOptions.binVersion) {
                otherParams.shardOptions.binVersion =
                    MongoRunner.versionIterator(otherParams.shardOptions.binVersion);
            }

            rsDefaults = Object.merge(rsDefaults, otherParams["d" + i]);
            rsDefaults = Object.merge(rsDefaults, otherParams.shardOptions);
        }

        rsDefaults.setParameter = rsDefaults.setParameter || {};
        rsDefaults.setParameter.migrationLockAcquisitionMaxWaitMS =
            otherParams.migrationLockAcquisitionMaxWaitMS;

        var rsSettings = rsDefaults.settings;
        delete rsDefaults.settings;

        // The number of nodes in the rs field will take priority.
        if (otherParams.rs || otherParams["rs" + i]) {
            var numReplicas = rsDefaults.nodes || 3;
        } else {
            var numReplicas = 1;
        }
        delete rsDefaults.nodes;

        var protocolVersion = rsDefaults.protocolVersion;
        delete rsDefaults.protocolVersion;

        var rs = new ReplSetTest({
            name: setName,
            nodes: numReplicas,
            host: hostName,
            useHostName: otherParams.useHostname,
            useBridge: otherParams.useBridge,
            bridgeOptions: otherParams.bridgeOptions,
            keyFile: this.keyFile,
            protocolVersion: protocolVersion,
            waitForKeys: false,
            settings: rsSettings,
            seedRandomNumberGenerator: !randomSeedAlreadySet,
        });

        print("ShardingTest starting replica set for shard: " + setName);

        // Start up the replica set but don't wait for it to complete. This allows the startup
        // of each shard to proceed in parallel.
        this._rs[i] =
            {setName: setName, test: rs, nodes: rs.startSetAsync(rsDefaults), url: rs.getURL()};
    }

    //
    // Start up the config server replica set.
    //

    var rstOptions = {
        useHostName: otherParams.useHostname,
        host: hostName,
        useBridge: otherParams.useBridge,
        bridgeOptions: otherParams.bridgeOptions,
        keyFile: this.keyFile,
        waitForKeys: false,
        name: testName + "-configRS",
        seedRandomNumberGenerator: !randomSeedAlreadySet,
        isConfigServer: true,
    };

    // always use wiredTiger as the storage engine for CSRS
    var startOptions = {
        pathOpts: pathOpts,
        // Ensure that journaling is always enabled for config servers.
        configsvr: "",
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

    // Start the config server's replica set without waiting for it to complete. This allows it
    // to proceed in parallel with the startup of each shard.
    this.configRS = new ReplSetTest(rstOptions);
    this.configRS.startSetAsync(startOptions);

    //
    // Wait for each shard replica set to finish starting up.
    //
    for (let i = 0; i < numShards; i++) {
        print("Waiting for shard " + this._rs[i].setName + " to finish starting up.");
        this._rs[i].test.startSetAwait();
    }

    //
    // Wait for the config server to finish starting up.
    //
    print("Waiting for the config server to finish starting up.");
    this.configRS.startSetAwait();
    var config = this.configRS.getReplSetConfig();
    config.configsvr = true;
    config.settings = config.settings || {};

    print("ShardingTest startup for all nodes took " + (new Date() - startTime) + "ms with " +
          this.configRS.nodeList().length + " config server nodes and " + totalNumShardNodes() +
          " total shard nodes.");

    //
    // Initiate each shard replica set and wait for replication. Also initiate the config replica
    // set. Whenever possible, in parallel.
    //
    const shardsRS = this._rs.map(obj => obj.test);
    const replicaSetsToInitiate = [...shardsRS, this.configRS].map(rst => {
        const rstConfig = rst.getReplSetConfig();

        // The mongo shell cannot authenticate as the internal __system user in tests that use x509
        // for cluster authentication. Choosing the default value for wcMajorityJournalDefault in
        // ReplSetTest cannot be done automatically without the shell performing such
        // authentication, so allow tests to pass the value in.
        if (otherParams.hasOwnProperty("writeConcernMajorityJournalDefault")) {
            rstConfig.writeConcernMajorityJournalDefault =
                otherParams.writeConcernMajorityJournalDefault;
        }

        if (rst === this.configRS) {
            rstConfig.configsvr = true;
            rstConfig.writeConcernMajorityJournalDefault = true;
        }

        return {
            rst,
            // Arguments for creating instances of each replica set within parallel threads.
            rstArgs: {
                name: rst.name,
                nodeHosts: rst.nodes.map(node => `127.0.0.1:${node.port}`),
                nodeOptions: rst.nodeOptions,
                // Mixed-mode SSL tests may specify a keyFile per replica set rather than one for
                // the whole cluster.
                keyFile: rst.keyFile ? rst.keyFile : this.keyFile,
                host: otherParams.useHostname ? hostName : "localhost",
                waitForKeys: false,
            },
            // Replica set configuration for initiating the replica set.
            rstConfig,
        };
    });

    const initiateReplicaSet = (rst, rstConfig) => {
        rst.initiateWithAnyNodeAsPrimary(rstConfig);

        // Do replication.
        rst.awaitNodesAgreeOnPrimary();
        if (rst.keyFile) {
            authutil.asCluster(rst.nodes, rst.keyFile, function() {
                rst.awaitReplication();
            });
        }
        rst.awaitSecondaryNodes();
    };

    const isParallelSupported = (() => {
        if (!tryLoadParallelTester()) {
            return false;
        }

        for (let {rst} of replicaSetsToInitiate) {
            if (rst.startOptions && rst.startOptions.clusterAuthMode === "x509") {
                // The mongo shell performing X.509 authentication as a cluster member requires
                // starting a parallel shell and using the server's (not the client's) certificate.
                // The ReplSetTest instance constructed in a Thread wouldn't have copied the path to
                // the server's certificate. We therefore fall back to initiating the CSRS and
                // replica set shards sequentially when X.509 authentication is being used.
                return false;
            }

            for (let n of Object.keys(rst.nodeOptions)) {
                const nodeOptions = rst.nodeOptions[n];
                if (nodeOptions && nodeOptions.clusterAuthMode === "x509") {
                    return false;
                }
            }
        }

        return true;
    })();

    if (isParallelSupported) {
        const threads = [];
        try {
            for (let {rstArgs, rstConfig} of replicaSetsToInitiate) {
                const thread = new Thread((rstArgs, rstConfig, initiateReplicaSet) => {
                    try {
                        const rst = new ReplSetTest({rstArgs});
                        initiateReplicaSet(rst, rstConfig);
                        return {ok: 1};
                    } catch (e) {
                        return {
                            ok: 0,
                            hosts: rstArgs.nodeHosts,
                            name: rstArgs.name,
                            error: e.toString(),
                            stack: e.stack,
                        };
                    }
                }, rstArgs, rstConfig, initiateReplicaSet);
                thread.start();
                threads.push(thread);
            }
        } finally {
            // Wait for each thread to finish. Throw an error if any thread fails.
            const returnData = threads.map(thread => {
                thread.join();
                return thread.returnData();
            });

            returnData.forEach(res => {
                assert.commandWorked(res,
                                     'Initiating shard or config servers as a replica set failed');
            });
        }
    } else {
        for (let {rst, rstConfig} of replicaSetsToInitiate) {
            initiateReplicaSet(rst, rstConfig);
        }
    }

    for (let i = 0; i < numShards; i++) {
        let rs = this._rs[i].test;

        this["rs" + i] = rs;
        this._rsObjects[i] = rs;

        this._connections.push(null);

        let rsConn = new Mongo(rs.getURL());
        rsConn.name = rs.getURL();

        this._connections[i] = rsConn;
        this["shard" + i] = rsConn;
        rsConn.rs = rs;
    }

    // Wait for master to be elected before starting mongos
    this.configRS.awaitNodesAgreeOnPrimary();
    var csrsPrimary = this.configRS.getPrimary();

    print("ShardingTest startup and initiation for all nodes took " + (new Date() - startTime) +
          "ms with " + this.configRS.nodeList().length + " config server nodes and " +
          totalNumShardNodes() + " total shard nodes.");

    // If 'otherParams.mongosOptions.binVersion' is an array value, then we'll end up constructing a
    // version iterator.
    const mongosOptions = [];
    for (var i = 0; i < numMongos; ++i) {
        let options = {
            useHostname: otherParams.useHostname,
            pathOpts: Object.merge(pathOpts, {mongos: i}),
            verbose: mongosVerboseLevel,
            keyFile: this.keyFile,
        };

        if (otherParams.mongosOptions && otherParams.mongosOptions.binVersion) {
            otherParams.mongosOptions.binVersion =
                MongoRunner.versionIterator(otherParams.mongosOptions.binVersion);
        }

        options = Object.merge(options, otherParams.mongosOptions);
        options = Object.merge(options, otherParams["s" + i]);

        // The default time for mongos quiesce mode in response to SIGTERM is 15 seconds.
        // Reduce this to 0 for faster shutdown.
        options.setParameter = options.setParameter || {};
        options.setParameter.mongosShutdownTimeoutMillisForSignaledShutdown =
            options.setParameter.mongosShutdownTimeoutMillisForSignaledShutdown || 0;

        options.port = options.port || _allocatePortForMongos();

        mongosOptions.push(options);
    }

    const configRS = this.configRS;
    const clusterVersionInfo = this.getClusterVersionInfo();
    if (_hasNewFeatureCompatibilityVersion() && clusterVersionInfo.isMixedVersion) {
        const fcv = binVersionToFCV(clusterVersionInfo.oldestBinVersion);
        function setFeatureCompatibilityVersion() {
            assert.commandWorked(csrsPrimary.adminCommand(
                {setFeatureCompatibilityVersion: fcv, fromConfigServer: true}));

            // Wait for the new featureCompatibilityVersion to propagate to all nodes in the CSRS
            // to ensure that older versions of mongos can successfully connect.
            configRS.awaitReplication();
        }

        if (this.keyFile) {
            authutil.asCluster(this.configRS.nodes, this.keyFile, setFeatureCompatibilityVersion);
        } else {
            setFeatureCompatibilityVersion();
        }
    }

    // If chunkSize has been requested for this test, write the configuration
    if (otherParams.chunkSize) {
        function setChunkSize() {
            assert.commandWorked(csrsPrimary.getDB('config').settings.update(
                {_id: 'chunksize'},
                {$set: {value: otherParams.chunkSize}},
                {upsert: true, writeConcern: {w: 'majority', wtimeout: kDefaultWTimeoutMs}}));

            configRS.awaitLastOpCommitted();
        }

        if (this.keyFile) {
            authutil.asCluster(csrsPrimary, this.keyFile, setChunkSize);
        } else {
            setChunkSize();
        }
    }

    this._configDB = this.configRS.getURL();
    for (var i = 0; i < numConfigs; ++i) {
        var conn = this.configRS.nodes[i];
        this["config" + i] = conn;
        this["c" + i] = conn;
    }

    printjson('Config servers: ' + this._configDB);

    print("ShardingTest " + this._testName + " :\n" +
          tojson({config: this._configDB, shards: this._connections}));

    this._mongos = [];

    // Start the MongoS servers
    for (var i = 0; i < numMongos; i++) {
        const options = mongosOptions[i];
        options.configdb = this._configDB;

        if (otherParams.useBridge) {
            var bridgeOptions =
                Object.merge(otherParams.bridgeOptions, options.bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: otherParams.useHostname ? hostName : "localhost",
                port: _allocatePortForBridgeForMongos(),
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

        if (otherParams.causallyConsistent) {
            conn.setCausalConsistency(true);
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
    if (this.keyFile) {
        authutil.asCluster(this._mongos, this.keyFile, _configureCluster);
    } else if (mongosOptions[0] && mongosOptions[0].keyFile) {
        authutil.asCluster(this._mongos, mongosOptions[0].keyFile, _configureCluster);
    } else {
        _configureCluster();
        // Ensure that all config server nodes are up to date with any changes made to balancer
        // settings before adding shards to the cluster. This prevents shards, which read
        // config.settings with readPreference 'nearest', from accidentally fetching stale values
        // from secondaries that aren't up-to-date.
        this.configRS.awaitLastOpCommitted();
    }

    try {
        if (!otherParams.manualAddShard) {
            var testName = this._testName;
            var admin = this.admin;

            this._connections.forEach(function(z) {
                var n = z.name || z.host || z;

                print("ShardingTest " + testName + " going to add shard : " + n);

                var result = assert.commandWorked(admin.runCommand({addshard: n}),
                                                  "Failed to add shard " + n);
                z.shardName = result.shardAdded;
            });
        }
    } catch (e) {
        // Clean up the running procceses on failure
        print("Failed to add shards, stopping cluster.");
        this.stop();
        throw e;
    }

    // Ensure that the sessions collection exists so jstests can run things with
    // logical sessions and test them. We do this by forcing an immediate cache refresh
    // on the config server, which auto-shards the collection for the cluster.
    this.configRS.getPrimary().getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});

    // Ensure that all CSRS nodes are up to date. This is strictly needed for tests that use
    // multiple mongoses. In those cases, the first mongos initializes the contents of the 'config'
    // database, but without waiting for those writes to replicate to all the config servers then
    // the secondary mongoses risk reading from a stale config server and seeing an empty config
    // database.
    this.configRS.awaitLastOpCommitted();

    if (jsTestOptions().keyFile) {
        jsTest.authenticateNodes(this._mongos);
    }

    // Flushes the routing table cache on connection 'conn'. If 'keyFileLocal' is defined,
    // authenticates the keyfile user.
    const flushRT = function flushRoutingTableAndHandleAuth(conn, keyFileLocal) {
        // Invokes the actual execution of cache refresh.
        const execFlushRT = (conn) => {
            assert.commandWorked(conn.getDB("admin").runCommand(
                {_flushRoutingTableCacheUpdates: "config.system.sessions"}));
        };

        const x509AuthRequired = (conn.fullOptions && conn.fullOptions.clusterAuthMode &&
                                  conn.fullOptions.clusterAuthMode === "x509");

        if (keyFileLocal) {
            authutil.asCluster(conn, keyFileLocal, () => execFlushRT(conn));
        } else if (x509AuthRequired) {
            const exitCode = _runMongoProgram(
                ...["mongo",
                    conn.host,
                    "--tls",
                    "--tlsAllowInvalidHostnames",
                    "--tlsCertificateKeyFile",
                    conn.fullOptions.tlsCertificateKeyFile ? conn.fullOptions.tlsCertificateKeyFile
                                                           : conn.fullOptions.sslPEMKeyFile,
                    "--tlsCAFile",
                    conn.fullOptions.tlsCAFile ? conn.fullOptions.tlsCAFile
                                               : conn.fullOptions.sslCAFile,
                    "--authenticationDatabase=$external",
                    "--authenticationMechanism=MONGODB-X509",
                    "--eval",
                    `(${execFlushRT.toString()})(db.getMongo())`,
            ]);
            assert.eq(0, exitCode, "parallel shell for x509 auth failed");
        } else {
            execFlushRT(conn);
        }
    };

    if (!otherParams.manualAddShard) {
        for (let i = 0; i < numShards; i++) {
            const keyFileLocal =
                (otherParams.shards && otherParams.shards[i] && otherParams.shards[i].keyFile)
                ? otherParams.shards[i].keyFile
                : this.keyFile;

            const rs = this._rs[i].test;
            flushRT(rs.getPrimary(), keyFileLocal);
        }

        self.waitForShardingInitialized();
    }
};

// Stub for a hook to check that collection UUIDs are consistent across shards and the config
// server.
ShardingTest.prototype.checkUUIDsConsistentAcrossCluster = function() {};

// Stub for a hook to check that indexes are consistent across shards.
ShardingTest.prototype.checkIndexesConsistentAcrossCluster = function() {};

ShardingTest.prototype.checkOrphansAreDeleted = function() {
    print("Unhooked function");
};

ShardingTest.prototype.checkRoutingTableConsistency = function() {
    print("Unhooked checkRoutingTableConsistency function");
};
