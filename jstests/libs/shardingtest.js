import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Symbol used to override the constructor. Please do not use this, it's only meant to aid
// in migrating the jstest corpus to proper module usage.
export const kOverrideConstructor = Symbol('overrideConstructor');

// Timeout to be used for operations scheduled by the sharding test, which must wait for write
// concern (5 minutes)
const kDefaultWTimeoutMs = 5 * 60 * 1000;

// Oplog collection name
const kOplogName = 'oplog.rs';

export class ShardingTest {
    // ShardingTest API
    getDB(name) {
        return this.s.getDB(name);
    }

    /**
     * Finds the _id of the primary shard for database 'dbname', e.g., 'test-rs0'
     */
    getPrimaryShardIdForDatabase(dbname) {
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
    }

    getNonPrimaries(dbname) {
        var x = this.config.databases.findOne({_id: dbname});
        if (!x) {
            this.config.databases.find().forEach(printjson);
            throw Error("couldn't find dbname: " + dbname +
                        " total: " + this.config.databases.count());
        }

        return this.config.shards.find({_id: {$ne: x.primary}}).map(z => z._id);
    }

    printNodes() {
        print("ShardingTest " + this._testName + " :\n" +
              tojson({config: this._configDB, shards: this._connections, mongos: this._mongos}));
    }

    getConnNames() {
        var names = [];
        for (var i = 0; i < this._connections.length; i++) {
            names.push(this._connections[i].name);
        }
        return names;
    }

    /**
     * Find the connection to the primary shard for database 'dbname'.
     */
    getPrimaryShard(dbname) {
        var dbPrimaryShardId = this.getPrimaryShardIdForDatabase(dbname);
        var primaryShard = this.config.shards.findOne({_id: dbPrimaryShardId});

        if (primaryShard) {
            const shardConnectionString = primaryShard.host;
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
    }

    normalize(x) {
        var z = this.config.shards.findOne({host: x});
        if (z)
            return z._id;
        return x;
    }

    /**
     * Find a different shard connection than the one given.
     */
    getOther(one) {
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
    }

    getAnother(one) {
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
    }

    stopAllConfigServers(opts, forRestart = undefined) {
        this.configRS.stopSet(undefined, forRestart, opts);
    }

    stopAllShards(opts = {}, forRestart = undefined) {
        if (isShutdownParallelSupported(this, opts)) {
            const threads = [];
            try {
                for (let {rstArgs} of replicaSetsToTerminate(this, this._rs)) {
                    const thread = new Thread(async (rstArgs, signal, forRestart, opts) => {
                        const {ReplSetTest} = await import("jstests/libs/replsettest.js");
                        try {
                            const rst = new ReplSetTest({rstArgs});
                            rst.stopSet(signal, forRestart, opts);
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
                    }, rstArgs, 15, forRestart, opts);

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
                                         'terminating shard or config server replica sets failed');
                });
            }
        } else {
            // The replica sets shutting down serially
            this._rs.forEach((rs) => {
                rs.test.stopSet(15, forRestart, opts);
            });
        }
    }

    stopAllMongos(opts) {
        for (var i = 0; i < this._mongos.length; i++) {
            this.stopMongos(i, opts);
        }
    }

    awaitMigrations() {
        this.stopBalancer();

        const numShards = this._rs.length;
        for (let i = 0; i < numShards; i++) {
            assert.commandWorked(this["shard" + i].adminCommand({"_shardsvrJoinMigrations": 1}));
        }
    }

    isReplicaSetEndpointActive() {
        const numShards = this._rs.length;
        return numShards == 1 && this._rs[0].test.isReplicaSetEndpointActive();
    }

    stop(opts = {}) {
        this.checkMetadataConsistency();
        this.checkUUIDsConsistentAcrossCluster();
        this.checkIndexesConsistentAcrossCluster();
        this.checkOrphansAreDeleted();
        this.checkRoutingTableConsistency();
        this.checkShardFilteringMetadata();

        if (jsTestOptions().alwaysUseLogFiles) {
            if (opts.noCleanData === false) {
                throw new Error("Always using log files, but received conflicting option.");
            }

            opts.noCleanData = true;
        }

        this.stopAllMongos(opts);

        if (TestData.runningWithConfigStepdowns && this.isConfigShardMode) {
            // In case of a cluster with a config shard, the config server replica set is stopped
            // via stopAllShards, which doesn't stop the continuous stepdown stop.
            this.configRS.stopContinuousFailover();
        }

        let startTime = new Date();  // Measure the execution time of shutting down shards.
        this.stopAllShards(opts);
        print("ShardingTest stopped all shards, took " + (new Date() - startTime) + "ms for " +
              this._connections.length + " shards.");

        if (!this.isConfigShardMode) {
            this.stopAllConfigServers(opts);
        }

        var timeMillis = new Date().getTime() - this._startTime.getTime();

        print('*** ShardingTest ' + this._testName + " completed successfully in " +
              (timeMillis / 1000) + " seconds ***");
    }

    stopOnFail() {
        try {
            this.stopAllMongos();
        } catch (e) {
            print("Did not successfully stop all mongos.");
        }
        try {
            this.stopAllShards();
        } catch (e) {
            print("Did not successfully stop all shards.");
        }
        try {
            this.stopAllConfigServers();
        } catch (e) {
            print("Did not successfully stop all config servers.");
        }
    }

    adminCommand(cmd) {
        var res = this.admin.runCommand(cmd);
        if (res && res.ok == 1)
            return true;

        throw _getErrorWithCode(res, "command " + tojson(cmd) + " failed: " + tojson(res));
    }

    restartAllConfigServers(opts) {
        this.configRS.startSet(opts, true);
        this.configRS.nodes.forEach((node) => {
            // node.routerPort is undefined if this node doesn't expose an embedded router, so this
            // loop only applies for embedded routers.
            const routerN = this._findRouterByPort(node.routerPort);
            if (routerN !== undefined) {
                this.reconnectToEmbeddedRouter(routerN);
            }
        });
        // We wait until a primary has been chosen since startSet can return without having elected
        // one. This can cause issues that expect a functioning replicaset once this method returns.
        this.configRS.waitForPrimary();
    }

    restartAllShards(opts) {
        this._rs.forEach((rs) => {
            rs.test.startSet(opts, true);
            rs.test.nodes.forEach((node) => {
                // node.routerPort is undefined if this node doesn't expose an embedded router, so
                // this loop only applies for embedded routers.
                const routerN = this._findRouterByPort(node.routerPort);
                if (routerN !== undefined) {
                    this.reconnectToEmbeddedRouter(routerN);
                }
            });
            // We wait until a primary has been chosen since startSet can return without having
            // elected one. This can cause issues that expect a functioning replicaset once this
            // method returns.
            rs.test.waitForPrimary();
        });
    }

    restartAllMongos(opts) {
        for (var i = 0; i < this._mongos.length; i++) {
            this.restartMongos(i, opts);
        }
    }

    forEachConnection(fn) {
        this._connections.forEach(function(conn) {
            fn(conn);
        });
    }

    forEachMongos(fn) {
        this._mongos.forEach(function(conn) {
            fn(conn);
        });
    }

    forEachConfigServer(fn) {
        this.configRS.nodes.forEach(function(conn) {
            fn(conn);
        });
    }

    printChangeLog() {
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
    }

    getChunksString(ns) {
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
    }

    printChunks(ns) {
        print("ShardingTest " + this.getChunksString(ns));
    }

    printShardingStatus(verbose) {
        printShardingStatus(this.config, verbose);
    }

    printCollectionInfo(ns, msg) {
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
    }

    /**
     * Returns the number of shards which contain the given dbName.collName collection
     */
    onNumShards(dbName, collName) {
        return this.shardCounts(dbName, collName)
            .reduce((total, currentValue) => total + (currentValue > 0 ? 1 : 0), 0);
    }

    /**
     * Returns an array of the size of numShards where each element is the number of documents on
     * that particular shard
     */
    shardCounts(dbName, collName) {
        return this._connections.map((connection) =>
                                         connection.getDB(dbName).getCollection(collName).count());
    }

    chunkCounts(collName, dbName) {
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
    }

    chunkDiff(collName, dbName) {
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
    }

    /**
     * Waits up to the specified timeout (with a default of 60s) for the collection to be
     * considered well balanced.
     **/
    awaitBalance(collName, dbName, timeToWait, interval) {
        const coll = this.s.getCollection(dbName + "." + collName);
        this.awaitCollectionBalance(coll, timeToWait, interval);
    }

    getShard(coll, query, includeEmpty) {
        var shards = this.getShardsForQuery(coll, query, includeEmpty);
        assert.eq(shards.length, 1);
        return shards[0];
    }

    /**
     * Returns the shards on which documents matching a particular query reside.
     */
    getShardsForQuery(coll, query, includeEmpty) {
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
    }

    shardColl(collName, key, split, move, dbName, waitForDelete) {
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

        for (var i = 0; i < 5; i++) {
            var otherShard = this.getOther(this.getPrimaryShard(dbName)).name;
            let cmd = {movechunk: c, find: move, to: otherShard};

            if (waitForDelete != null) {
                cmd._waitForDelete = waitForDelete;
            }

            const result = this.s.adminCommand(cmd);
            if (result.ok)
                break;

            sleep(5 * 1000);
        }

        assert.commandWorked(result);
    }

    /**
     * Wait for sharding to be initialized.
     */
    waitForShardingInitialized(timeoutMs = 60 * 1000) {
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
            const nodes = replSet.test.nodes;
            const keyFileUsed = replSet.test.keyFile;

            for (var j = 0; j < nodes.length; ++j) {
                const diff = (new Date()).getTime() - start.getTime();
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

                const x509AuthRequired =
                    (this.s.fullOptions && this.s.fullOptions.clusterAuthMode &&
                     this.s.fullOptions.clusterAuthMode === "x509");

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
    }

    /**
     * Kills the mongos with index n.
     *
     * @param {boolean} [extraOptions.waitPid=true] if true, we will wait for the process to
     * terminate after stopping it.
     */
    stopMongos(n, opts, {
        waitpid: waitpid = true,
    } = {}) {
        if (this._useBridge) {
            MongoRunner.stopMongos(this._unbridgedMongos[n], undefined, opts, waitpid);
            this["s" + n].stop();
        } else {
            let mongos = this["s" + n];
            // this isn't a real mongos, it's the embedded router of a mongod. Don't do anything.
            if (mongos.isEmbeddedRouter) {
                return;
            }
            MongoRunner.stopMongos(mongos, undefined, opts, waitpid);
        }
    }

    /**
     * Kills the config server mongod with index n.
     */
    stopConfigServer(n, opts) {
        this.configRS.stop(n, undefined, opts);
    }

    /**
     * Stops and restarts a mongos process. The operation fails if this connection is not against a
     * mongos process (i.e. an embedded router).
     *
     * If 'opts' is not specified, starts the mongos with its previous parameters.  If 'opts' is
     * specified and 'opts.restart' is false or missing, starts mongos with the parameters specified
     * in 'opts'.  If opts is specified and 'opts.restart' is true, merges the previous options
     * with the options specified in 'opts', with the options in 'opts' taking precedence.
     *
     * Warning: Overwrites the old s (if n = 0) admin, config, and sn member variables.
     */
    restartMongos(n, opts) {
        var mongos;

        if (this._useBridge) {
            mongos = this._unbridgedMongos[n];
        } else {
            mongos = this["s" + n];
        }

        assert(!mongos.isEmbeddedRouter,
               "This mongos is an embedded router, it can't be restarted separately");

        opts = opts || mongos;
        opts.port = opts.port || mongos.port;

        this.stopMongos(n);

        if (this._useBridge) {
            const hostName =
                this._otherParams.host === undefined ? getHostName() : this._otherParams.host;
            var bridgeOptions =
                (opts !== mongos) ? opts.bridgeOptions : mongos.fullOptions.bridgeOptions;
            bridgeOptions = Object.merge(this._otherParams.bridgeOptions, bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: this._otherParams.useHostname ? hostName : "localhost",
                port: this._mongos[n].port,
                // The mongos processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + opts.port,
            });

            this._mongos[n] = new MongoBridge(bridgeOptions);
        }

        if (opts.restart) {
            opts = Object.merge(mongos.fullOptions, opts);
        }

        var newConn = MongoRunner.runMongos(opts);
        if (!newConn) {
            throw new Error("Failed to restart mongos " + n);
        }

        if (this._useBridge) {
            this._mongos[n].connectToBridge();
            this._unbridgedMongos[n] = newConn;
        } else {
            this._mongos[n] = newConn;
        }

        this['s' + n] = this._mongos[n];
        if (n == 0) {
            this.s = this._mongos[n];
            this.admin = this._mongos[n].getDB('admin');
            this.config = this._mongos[n].getDB('config');
        }
    }

    /**
     * Restarts a router node. The node can be either a standalone mongoS, or a mongoD with an
     * embedded router. If the node is a mongoD with embedded router, this command will wait until
     * the triggered election finishes. As a consequence, the primary of the affected RS could
     * change.
     *
     * If 'opts' is not specified, starts the node with its previous parameters.  If 'opts' is
     * specified and 'opts.restart' is false or missing, starts the node with the parameters
     * specified in 'opts'.  If opts is specified and 'opts.restart' is true, merges the previous
     * options with the options specified in 'opts', with the options in 'opts' taking precedence.
     *
     * Warning: Overwrites the old s (if n = 0) admin, config, and sn member variables.
     */
    restartRouterNode(n, opts) {
        const routerConn = (() => {
            if (this._useBridge) {
                return this._unbridgedMongos[n];
            } else {
                return this["s" + n];
            }
        })();

        if (!routerConn.isEmbeddedRouter) {
            this.restartMongos(n, opts);
            return;
        }

        assert(!this._useBridge, "BUG: mongobridge and embedded router are not compatible");

        const nodeInfo = routerConn.nodeInfo;

        if (nodeInfo.isConfig) {
            this.restartConfigServer(nodeInfo.index, opts);
        } else {
            nodeInfo.rs.restart(nodeInfo.index, opts);
        }

        this.reconnectToEmbeddedRouter(n);

        // Wait for any election to succeed.
        nodeInfo.rs.awaitNodesAgreeOnPrimary();
    }

    reconnectToEmbeddedRouter(n) {
        const routerConn = (() => {
            if (this._useBridge) {
                return this._unbridgedMongos[n];
            } else {
                return this["s" + n];
            }
        })();
        const nodeInfo = routerConn.nodeInfo;

        const mongodConn = nodeInfo.rs.nodes[nodeInfo.index];
        const newConn =
            MongoRunner.awaitConnection({pid: mongodConn.pid, port: mongodConn.routerPort});
        newConn.isEmbeddedRouter = true;
        newConn.port = mongodConn.routerPort;
        newConn.nodeInfo = nodeInfo;
        newConn.fullOptions = mongodConn.fullOptions;
        newConn.commandLine = mongodConn.commandLine;
        newConn.name = routerConn.name;
        newConn.host = routerConn.host;

        this._mongos[n] = newConn;
        this["s" + n] = newConn;

        if (n == 0) {
            this.s = this._mongos[n];
            this.admin = this._mongos[n].getDB('admin');
            this.config = this._mongos[n].getDB('config');
        }
    }

    /** @private */
    _findRouterByPort(port) {
        if (port === undefined) {
            return undefined;
        }
        for (let n = 0; n < this._mongos.length; n++) {
            if (this._mongos[n].port == port) {
                return n;
            }
        }
        return undefined;
    }

    /**
     * Shuts down and restarts replica set for a given shard and
     * updates shard connection information.
     *
     * @param {string} prevShardName
     * @param {object} replSet The replica set object. Defined in replsettest.js
     */
    shutdownAndRestartPrimaryOnShard(shardName, replSet) {
        const n = this._shardReplSetToIndex[replSet.name];
        const originalPrimaryConn = replSet.getPrimary();

        const SIGTERM = 15;
        replSet.restart(originalPrimaryConn, {}, SIGTERM);
        replSet.awaitNodesAgreeOnPrimary();
        replSet.awaitSecondaryNodes();

        this._connections[n] = new Mongo(replSet.getURL());
        this._connections[n].shardName = shardName;
        this._connections[n].rs = replSet;

        this["shard" + n] = this._connections[n];
    }

    /**
     * Kills and restarts replica set for a given shard and
     * updates shard connection information.
     *
     * @param {string} prevShardName
     * @param {object} replSet The replica set object. Defined in replsettest.js
     */
    killAndRestartPrimaryOnShard(shardName, replSet) {
        const n = this._shardReplSetToIndex[replSet.name];
        const originalPrimaryConn = replSet.getPrimary();

        const SIGKILL = 9;
        const opts = {allowedExitCode: MongoRunner.EXIT_SIGKILL};
        replSet.restart(originalPrimaryConn, opts, SIGKILL);
        replSet.awaitNodesAgreeOnPrimary();

        this._connections[n] = new Mongo(replSet.getURL());
        this._connections[n].shardName = shardName;
        this._connections[n].rs = replSet;

        this["shard" + n] = this._connections[n];
    }

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
    restartShardRS(n, options, signal, wait) {
        const prevShardName = this._connections[n].shardName;
        for (let i = 0; i < this["rs" + n].nodeList().length; i++) {
            this["rs" + n].restart(i);
        }

        this["rs" + n].awaitSecondaryNodes();
        this._connections[n] = new Mongo(this["rs" + n].getURL(), undefined, {gRPC: false});
        this._connections[n].shardName = prevShardName;
        this._connections[n].rs = this["rs" + n];
        this["shard" + n] = this._connections[n];
    }

    /**
     * Stops and restarts a config server mongod process.
     *
     * If opts is specified, the new mongod is started using those options. Otherwise, it is
     * started
     * with its previous parameters.
     *
     * Warning: Overwrites the old cn/confign member variables.
     */
    restartConfigServer(n, options, signal, wait) {
        this.configRS.restart(n, options, signal, wait);
        this["config" + n] = this.configRS.nodes[n];
        this["c" + n] = this.configRS.nodes[n];
    }

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
     *     jsTestOptions().mongosBinVersion,
     *     otherParams.configOptions.binVersion,
     *     otherParams.shardOptions.binVersion,
     *     otherParams.mongosOptions.binVersion
     */
    getClusterVersionInfo() {
        let hasLastLTS = clusterHasBinVersion(this, "last-lts");
        let hasLastContinuous = clusterHasBinVersion(this, "last-continuous");
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
    }

    /**
     * Runs a find on the namespace to force a refresh of the node's catalog cache.
     */
    refreshCatalogCacheForNs(node, ns) {
        node.getCollection(ns).findOne();
    }

    /**
     * Waits for all operations to fully replicate on all shards.
     */
    awaitReplicationOnShards() {
        this._rs.forEach(replSet => replSet.test.awaitReplication());
    }

    /**
     * Query the oplog from a given node.
     */
    findOplog(conn, query, limit) {
        return conn.getDB('local')
            .getCollection(kOplogName)
            .find(query)
            .sort({$natural: -1})
            .limit(limit);
    }

    /**
     * Returns all nodes in the cluster including shards, config servers and mongoses.
     */
    getAllNodes() {
        let nodes = [];
        nodes.concat([this._configDB, this._connections, this._mongos]);
        return [...new Set(nodes)];
    }

    /**
     * @typedef {Object} ShardingTestOtherParams
     * @property {Object} [rs] Same `rs` parameter to ShardingTest constructor
     * @property {number} [chunkSize] Same as chunkSize parameter to ShardingTest constructor
     * @property {string} [keyFile] The location of the keyFile
     * @property {Object} [shardOptions] Same as the `shards` parameter to ShardingTest constructor.
     * Can be used to specify options that are common all shards.
     * @property {Object} [configOptions] Same as the `config` parameter to ShardingTest
     * constructor. Can be used to specify options that are common all config servers.
     * @property {Object} [mongosOptions] Same as the `mongos` parameter to ShardingTest
     * constructor. Can be used to specify options that are common all mongos.
     * @property {boolean} [enableBalancer] If true, enable the balancer enableBalancer setting
     * @property {boolean} [manualAddShard] Shards will not be added if true.
     * @property {number} [migrationLockAcquisitionMaxWaitMS] Number of milliseconds to acquire the
     * migration lock.
     * @property {boolean} [useBridge=false] If true, then a mongobridge process is started for each
     * node in the sharded cluster.
     * @property {boolean} [causallyConsistent=false] Specifies whether the connections to the
     * replica set nodes should be created with the 'causal consistency' flag enabled, which means
     * they will gossip the cluster time and add readConcern afterClusterTime where applicable.
     * @property {Object} [bridgeOptions={}] Options to apply to all mongobridge processes.
     *
     * // replica Set only:
     * @property {Object} [rsOptions] Same as the `rs` parameter to ShardingTest constructor. Can be
     * used to specify options that are common all replica members.
     * @property {boolean} [useHostname] if true, use hostname of machine, otherwise use localhost
     * @property {number} [numReplicas]
     * @property {boolean} [configShard] Add the config server as a shard if true.
     * @property {boolean} [useAutoBootstrapProcedure] Use the auto-bootstrapping procedure on every
     * shard and config server if set to true.
     * @property {boolean} [alwaysUseTestNameForShardName] Always use the testname as the name of
     * the shard.
     * @property {boolean} [embeddedRouter] Use mongod's embedding routing functionality instead of
     * mongos. Each st.s, s0, s1,.... points to the router port of a mongod instead of a dedicated
     * mongos. Chooses `numMongos` mongod nodes from the shards to act as routers, at random.
     * Incompatible with useBridge.
     */

    /**
     * @typedef {Object} ReplSetConfig
     * @property {number} [nodes=3] Number of replica members.
     * @property {number} [protocolVersion] Protocol version of replset used by the replset
     * initiation. For other options, @see ReplSetTest#initiate
     */

    /**
     * Starts up a sharded cluster with the given specifications. The cluster will be fully
     * operational after the execution of this constructor function.
     *
     * In addition to its own methods, ShardingTest inherits all the functions from the 'sh' utility
     * with the db set as the first mongos instance in the test (i.e. s0).
     *
     * @param {Object} params Contains the key-value pairs for the cluster configuration.
     * @param {string} [params.name] Name for this test
     * @param {boolean} [params.shouldFailInit] If set, assert that this will fail initialization
     * @param {number} [params.verbose] The verbosity for the mongos
     * @param {number} [params.chunkSize] The chunk size to use as configuration for the cluster
     * @param {number|Object|Array.<Object>} [params.mongos] Number of mongos or mongos
     *     configuration object(s)(*). see MongoRunner.runMongos
     * @param {ReplSetConfig|Array.<ReplSetConfig>} [params.rs] Replica set configuration object.
     * @param {number|Object|Array.<Object>} [params.shards] Number of shards or shard configuration
     *     object(s)(*). see MongoRunner.runMongod
     * @param {number|Object|Array.<Object>} [params.config] Number of config server or config
     *     server configuration object(s)(*). see MongoRunner.runMongod
     *     (*) There are two ways For multiple configuration objects.
     *       (1) Using the object format. Example:
     *           { d0: { verbose: 5 }, d1: { auth: '' }, rs2: { oplogsize: 10 }}
     *           In this format, d = mongod, s = mongos & c = config servers
     *
     *       (2) Using the array format. Example:
     *           [{ verbose: 5 }, { auth: '' }]
     *
     *       Note: you can only have single server shards for array format.
     *       Note: A special "bridgeOptions" property can be specified in both the object and array
     *          formats to configure the options for the mongobridge corresponding to that node.
     * These options are merged with the params.bridgeOptions options, where the node-specific
     *          options take precedence.
     * @param {ShardingTestOtherParams} [params.other] Other parameters
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
    constructor(params) {
        if (!(this instanceof ShardingTest)) {
            return new ShardingTest(params);
        }

        if (this.constructor === ReplSetTest && this.constructor[kOverrideConstructor]) {
            return new this.constructor[kOverrideConstructor];
        }

        // Ensure we don't mutate the passed-in parameters.
        params = Object.extend({}, params, true);

        // Used for counting the test duration
        this._startTime = new Date();

        assert(isObject(params), 'ShardingTest configuration must be a JSON object');

        var testName = params.name || jsTest.name();
        var otherParams = Object.deepMerge(params, params.other || {});
        var numShards = otherParams.hasOwnProperty('shards') ? otherParams.shards : 2;
        var mongosVerboseLevel = otherParams.hasOwnProperty('verbose') ? otherParams.verbose : 1;
        var numMongos = otherParams.hasOwnProperty('mongos') ? otherParams.mongos : 1;
        const usedDefaultNumConfigs =
            !otherParams.hasOwnProperty('config') || otherParams.config === undefined;
        var numConfigs = otherParams.hasOwnProperty('config') ? otherParams.config : 3;

        let useAutoBootstrapProcedure = otherParams.hasOwnProperty('useAutoBootstrapProcedure')
            ? otherParams.useAutoBootstrapProcedure
            : false;
        useAutoBootstrapProcedure =
            useAutoBootstrapProcedure || jsTestOptions().useAutoBootstrapProcedure;
        let alwaysUseTestNameForShardName =
            otherParams.hasOwnProperty('alwaysUseTestNameForShardName')
            ? otherParams.alwaysUseTestNameForShardName
            : false;

        let isConfigShardMode =
            otherParams.hasOwnProperty('configShard') ? otherParams.configShard : false;
        isConfigShardMode =
            isConfigShardMode || jsTestOptions().configShard || useAutoBootstrapProcedure;
        Object.defineProperty(
            this,
            "isConfigShardMode",
            {value: isConfigShardMode, writable: false, enumerable: true, configurable: false});

        let isEmbeddedRouterMode =
            otherParams.hasOwnProperty('embeddedRouter') ? otherParams.embeddedRouter : false;
        isEmbeddedRouterMode = isEmbeddedRouterMode || jsTestOptions().embeddedRouter;

        if (isEmbeddedRouterMode) {
            assert(!otherParams.useBridge,
                   "Embedded router mode is not compatible with mongobridge");
        }

        if ("shardAsReplicaSet" in otherParams) {
            throw new Error("Use of deprecated option 'shardAsReplicaSet'");
        }

        // Default enableBalancer to false.
        otherParams.enableBalancer =
            ("enableBalancer" in otherParams) && (otherParams.enableBalancer === true);

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
        defineReadOnlyProperty(this, "_numShards", numShards);

        if (isConfigShardMode) {
            assert(numShards > 0, 'Config shard mode requires at least one shard');
        }

        if (Array.isArray(numMongos)) {
            for (var i = 0; i < numMongos.length; i++) {
                otherParams["s" + i] = numMongos[i];
            }

            numMongos = numMongos.length;
        } else if (isObject(numMongos)) {
            let tempCount = 0;
            for (var i in numMongos) {
                otherParams[i] = numMongos[i];
                tempCount++;
            }

            numMongos = tempCount;
        }
        defineReadOnlyProperty(this, "_numMongos", numMongos);

        if (Array.isArray(numConfigs)) {
            assert(!usedDefaultNumConfigs);
            for (let i = 0; i < numConfigs.length; i++) {
                otherParams["c" + i] = numConfigs[i];
            }

            numConfigs = numConfigs.length;
        } else if (isObject(numConfigs)) {
            assert(!usedDefaultNumConfigs);
            let tempCount = 0;
            for (var i in numConfigs) {
                otherParams[i] = numConfigs[i];
                tempCount++;
            }

            numConfigs = tempCount;
        }
        defineReadOnlyProperty(this, "_numConfigs", numConfigs);

        otherParams.useHostname =
            otherParams.useHostname == undefined ? true : otherParams.useHostname;
        otherParams.useBridge = otherParams.useBridge || false;
        otherParams.bridgeOptions = otherParams.bridgeOptions || {};
        otherParams.causallyConsistent = otherParams.causallyConsistent || false;

        if (jsTestOptions().networkMessageCompressors) {
            otherParams.bridgeOptions["networkMessageCompressors"] =
                jsTestOptions().networkMessageCompressors;
        }

        this.keyFile = otherParams.keyFile;
        const hostName = otherParams.host === undefined ? getHostName() : otherParams.host;

        this._testName = testName;
        this._otherParams = otherParams;

        var pathOpts = {testName: testName};

        this._connections = [];
        this._rs = [];
        this._rsObjects = [];
        this._shardReplSetToIndex = {};

        this._useBridge = otherParams.useBridge;
        if (this._useBridge) {
            assert(
                !jsTestOptions().tlsMode,
                'useBridge cannot be true when using TLS. Add the requires_mongobridge tag to the test to ensure it will be skipped on variants that use TLS.');
        }

        this._unbridgedMongos = [];
        let _allocatePortForMongos;
        let _allocatePortForBridgeForMongos;

        if (this._useBridge) {
            this._unbridgedMongos = [];

            const _makeAllocatePortFn = (preallocatedPorts, errorMessage) => {
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

            const errorMessage = (length) =>
                "Cannot use more than " + length + " mongos processes when useBridge=true";
            _allocatePortForBridgeForMongos =
                _makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset), errorMessage);
            _allocatePortForMongos =
                _makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset), errorMessage);
        } else {
            _allocatePortForBridgeForMongos = function() {
                throw new Error("Using mongobridge isn't enabled for this sharded cluster");
            };
            _allocatePortForMongos = allocatePort;
        }

        otherParams.migrationLockAcquisitionMaxWaitMS =
            otherParams.migrationLockAcquisitionMaxWaitMS || 30000;

        let randomSeedAlreadySet = false;

        if (jsTest.options().useRandomBinVersionsWithinReplicaSet) {
            // We avoid setting the random seed unequivocally to avoid unexpected behavior in tests
            // that already make use of Random.setRandomSeed(). This conditional can be removed if
            // it becomes the standard to always be generating the seed through ShardingTest.
            Random.setRandomFixtureSeed();
            randomSeedAlreadySet = true;
        }

        try {
            const clusterVersionInfo = this.getClusterVersionInfo();

            let startTime = new Date();  // Measure the execution time of startup and initiate.
            if (!isConfigShardMode) {
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
                    isRouterServer: isEmbeddedRouterMode,
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
                    if (isEmbeddedRouterMode && otherParams.mongosOptions) {
                        nodeOptions[i] = Object.merge(otherParams.mongosOptions, nodeOptions[i]);
                        if (otherParams.mongosOptions.setParameter) {
                            nodeOptions[i].setParameter =
                                Object.merge(otherParams.mongosOptions.setParameter,
                                             nodeOptions[i].setParameter);
                        }
                    }
                }

                rstOptions.nodes = nodeOptions;

                // Start the config server's replica set without waiting for it to complete. This
                // allows it to proceed in parallel with the startup of each shard.
                this.configRS = new ReplSetTest(rstOptions);
                this.configRS.startSetAsync(startOptions);
            }

            //
            // Start each shard replica set.
            //
            for (var i = 0; i < numShards; i++) {
                var setName = testName + "-rs" + i;

                var rsDefaults = {
                    useHostname: otherParams.useHostname,
                    oplogSize: 16,
                    pathOpts: Object.merge(pathOpts, {shard: i}),
                };

                var setIsConfigSvr = false;

                if (isConfigShardMode && i == 0) {
                    otherParams.configOptions = Object.merge(
                        otherParams.configOptions, {configsvr: "", storageEngine: "wiredTiger"});

                    rsDefaults = Object.merge(rsDefaults, otherParams.configOptions);
                    setIsConfigSvr = true;
                } else {
                    rsDefaults.shardsvr = '';
                }

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
                    if (otherParams.shardOptions && otherParams.shardOptions.binVersion) {
                        otherParams.shardOptions.binVersion =
                            MongoRunner.versionIterator(otherParams.shardOptions.binVersion);
                    }

                    rsDefaults = Object.merge(rsDefaults, otherParams["d" + i]);
                    rsDefaults = Object.merge(rsDefaults, otherParams.shardOptions);
                }

                rsDefaults.setParameter = rsDefaults.setParameter || {};

                if (typeof (rsDefaults.setParameter) === "string") {
                    var eqIdx = rsDefaults.setParameter.indexOf("=");
                    if (eqIdx != -1) {
                        var param = rsDefaults.setParameter.substring(0, eqIdx);
                        var value = rsDefaults.setParameter.substring(eqIdx + 1);
                        rsDefaults.setParameter = {};
                        rsDefaults.setParameter[param] = value;
                    }
                }

                rsDefaults.setParameter.migrationLockAcquisitionMaxWaitMS =
                    otherParams.migrationLockAcquisitionMaxWaitMS;

                var rsSettings = rsDefaults.settings;
                delete rsDefaults.settings;

                // The number of nodes in the rs field will take priority.
                let numReplicas;
                if (otherParams.rs || otherParams["rs" + i]) {
                    numReplicas = rsDefaults.nodes || 3;
                } else {
                    numReplicas = 1;
                }

                // Unless explicitly given a number of config servers, a config shard uses the
                // shard's number of nodes to increase odds of compatibility with test assertions.
                if (isConfigShardMode && i == 0 && !usedDefaultNumConfigs) {
                    numReplicas = numConfigs;
                }

                delete rsDefaults.nodes;

                var protocolVersion = rsDefaults.protocolVersion;
                delete rsDefaults.protocolVersion;

                let replSetTestOpts = {
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
                    isConfigServer: setIsConfigSvr,
                    isRouterServer: isEmbeddedRouterMode,
                    useAutoBootstrapProcedure: useAutoBootstrapProcedure,
                };

                if (isEmbeddedRouterMode && otherParams.mongosOptions) {
                    replSetTestOpts = Object.merge(otherParams.mongosOptions, replSetTestOpts);
                    if (otherParams.mongosOptions.setParameter) {
                        replSetTestOpts.setParameter = Object.merge(
                            otherParams.mongosOptions.setParameter, replSetTestOpts.setParameter);
                    }
                }

                const rs = new ReplSetTest(replSetTestOpts);

                print("ShardingTest starting replica set for shard: " + setName);

                // Start up the replica set but don't wait for it to complete. This allows the
                // startup of each shard to proceed in parallel.
                this._rs[i] = {
                    setName: setName,
                    test: rs,
                    nodes: rs.startSetAsync(rsDefaults),
                    url: rs.getURL()
                };
                if (i == 0 && isConfigShardMode) {
                    this.configRS = this._rs[0].test;
                }
            }

            //
            // Wait for each shard replica set to finish starting up.
            //
            for (let i = 0; i < numShards; i++) {
                print("Waiting for shard " + this._rs[i].setName + " to finish starting up.");
                if (isConfigShardMode && i == 0) {
                    continue;
                }
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

            print("ShardingTest startup for all nodes took " + (new Date() - startTime) +
                  "ms with " + this.configRS.nodeList().length + " config server nodes and " +
                  totalNumShardNodes(this) + " total shard nodes.");

            //
            // Initiate each shard replica set and wait for replication. Also initiate the config
            // replica set. Whenever possible, in parallel.
            //
            const shardsRS = this._rs.map(obj => obj.test);
            var replSetToIntiateArr = [];
            if (isConfigShardMode) {
                replSetToIntiateArr = [...shardsRS];
            } else {
                replSetToIntiateArr = [...shardsRS, this.configRS];
            }

            const replicaSetsToInitiate = replSetToIntiateArr.map(rst => {
                const rstConfig = rst.getReplSetConfig();

                // The mongo shell cannot authenticate as the internal __system user in tests that
                // use x509 for cluster authentication. Choosing the default value for
                // wcMajorityJournalDefault in ReplSetTest cannot be done automatically without the
                // shell performing such authentication, so allow tests to pass the value in.
                if (otherParams.hasOwnProperty("writeConcernMajorityJournalDefault")) {
                    rstConfig.writeConcernMajorityJournalDefault =
                        otherParams.writeConcernMajorityJournalDefault;
                }

                if (rst === this.configRS) {
                    rstConfig.configsvr = true;
                    rstConfig.writeConcernMajorityJournalDefault = true;
                }

                const makeNodeHost = (node) => {
                    const [_, port] = node.name.split(":");
                    return `127.0.0.1:${port}`;
                };
                return {
                    rst,
                    // Arguments for creating instances of each replica set within parallel threads.
                    rstArgs: {
                        name: rst.name,
                        nodeHosts: rst.nodes.map(node => makeNodeHost(node)),
                        nodeOptions: rst.nodeOptions,
                        // Mixed-mode SSL tests may specify a keyFile per replica set rather than
                        // one for the whole cluster.
                        keyFile: rst.keyFile ? rst.keyFile : this.keyFile,
                        host: otherParams.useHostname ? hostName : "localhost",
                        waitForKeys: false,
                        useAutoBootstrapProcedure: useAutoBootstrapProcedure,
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
                for (let {rst} of replicaSetsToInitiate) {
                    if (rst.startOptions && rst.startOptions.clusterAuthMode === "x509") {
                        // The mongo shell performing X.509 authentication as a cluster member
                        // requires starting a parallel shell and using the server's (not the
                        // client's) certificate. The ReplSetTest instance constructed in a Thread
                        // wouldn't have copied the path to the server's certificate. We therefore
                        // fall back to initiating the CSRS and replica set shards sequentially when
                        // X.509 authentication is being used.
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
                        const thread =
                            new Thread(async (rstArgs, rstConfig, initiateReplicaSet) => {
                                const {ReplSetTest} = await import("jstests/libs/replsettest.js");
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
                        assert.commandWorked(
                            res, 'Initiating shard or config servers as a replica set failed');
                    });
                }
            } else {
                for (let {rst, rstConfig} of replicaSetsToInitiate) {
                    initiateReplicaSet(rst, rstConfig);
                }
            }

            for (let i = 0; i < numShards; i++) {
                let rs = this._rs[i].test;

                this._shardReplSetToIndex[rs.name] = i;
                this["rs" + i] = rs;
                this._rsObjects[i] = rs;

                this._connections.push(null);

                let rsConn = new Mongo(rs.getURL(), undefined, {gRPC: false});
                rsConn.name = rs.getURL();

                this._connections[i] = rsConn;
                this["shard" + i] = rsConn;
                rsConn.rs = rs;
            }

            // Wait for master to be elected before starting mongos
            this.configRS.awaitNodesAgreeOnPrimary();
            var csrsPrimary = this.configRS.getPrimary();

            // TODO: SERVER-80010 Remove assert.soon.
            if (useAutoBootstrapProcedure) {
                assert.soonNoExcept(() => {
                    function isShardingReady() {
                        return csrsPrimary.adminCommand({getShardingReady: 1}).isReady;
                    }
                    return this.keyFile
                        ? authutil.asCluster(csrsPrimary, this.keyFile, isShardingReady)
                        : isShardingReady();
                });
            }

            print("ShardingTest startup and initiation for all nodes took " +
                  (new Date() - startTime) + "ms with " + this.configRS.nodeList().length +
                  " config server nodes and " + totalNumShardNodes(this) + " total shard nodes.");

            // If 'otherParams.mongosOptions.binVersion' is an array value, then we'll end up
            // constructing a version iterator.
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
                if (jsTestOptions().shellGRPC) {
                    options.grpcPort = options.grpcPort || _allocatePortForMongos();
                }

                mongosOptions.push(options);
            }

            const configRS = this.configRS;
            if (_hasNewFeatureCompatibilityVersion() && clusterVersionInfo.isMixedVersion) {
                const fcv = binVersionToFCV(clusterVersionInfo.oldestBinVersion);
                function setFeatureCompatibilityVersion() {
                    assert.commandWorked(csrsPrimary.adminCommand({
                        setFeatureCompatibilityVersion: fcv,
                        confirm: true,
                        fromConfigServer: true
                    }));

                    // Wait for the new featureCompatibilityVersion to propagate to all nodes in the
                    // CSRS to ensure that older versions of mongos can successfully connect.
                    configRS.awaitReplication();
                }

                if (this.keyFile) {
                    authutil.asCluster(
                        this.configRS.nodes, this.keyFile, setFeatureCompatibilityVersion);
                } else {
                    setFeatureCompatibilityVersion();
                }
            }

            // If chunkSize has been requested for this test, write the configuration
            if (otherParams.chunkSize) {
                function setChunkSize() {
                    assert.commandWorked(csrsPrimary.getDB('config').settings.update(
                        {_id: 'chunksize'}, {$set: {value: otherParams.chunkSize}}, {
                            upsert: true,
                            writeConcern: {w: 'majority', wtimeout: kDefaultWTimeoutMs}
                        }));

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

            this.printNodes();

            this._mongos = [];

            // Start and connect to the MongoS servers if needed; create connections to the embedded
            // router ports if not.
            if (isEmbeddedRouterMode) {
                print("Connecting to embedded routers...");

                let allShardNodes = this._rs
                                        .map(r => r.test.nodes.map((_, index) => ({
                                                                       rs: r.test,
                                                                       index: index,
                                                                       isConfig: false,
                                                                   })))
                                        .flat();
                if (!isConfigShardMode) {
                    allShardNodes =
                        allShardNodes.concat(this.configRS.nodes.map((_, index) => ({
                                                                         rs: this.configRS,
                                                                         index: index,
                                                                         isConfig: true,
                                                                     })));
                }

                assert(allShardNodes.length >= numMongos,
                       'Need at least numMongos total mongod nodes in the cluster');

                if (!randomSeedAlreadySet) {
                    Random.setRandomFixtureSeed();
                }

                // TODO (SERVER-87462): Randomize the list of routers to choose after adding shards
                // from the router port.

                // Make sure to push one configsvr router port as the first one so it will be the
                // one responsible to run the addShard commands, extend all sh methods and stop the
                // balancer.
                const shuffledNodes = Array.shuffle(allShardNodes);
                const index =
                    shuffledNodes.findIndex(node => (node.rs === this.configRS && node.index == 0));
                assert(index != -1, "Can't find first node of the config server");
                const configNode = shuffledNodes.splice(index, 1)[0];
                shuffledNodes.unshift(configNode);

                let routerNodes = shuffledNodes.slice(0, numMongos);
                let i = 0;

                print("Chose the following nodes to act as embedded routers: " + routerNodes);
                for (const nodeInfo of routerNodes) {
                    const node = nodeInfo.rs.nodes[nodeInfo.index];
                    const conn =
                        MongoRunner.awaitConnection({pid: node.pid, port: node.routerPort});
                    conn.isEmbeddedRouter = true;
                    conn.port = node.routerPort;
                    conn.nodeInfo = nodeInfo;
                    conn.fullOptions = node.fullOptions;
                    conn.name = MongoRunner.getMongosName(node.routerPort, otherParams.useHostname);
                    conn.host = conn.name;
                    conn.commandLine = node.commandLine;
                    this._mongos.push(conn);
                    this["s" + i++] = conn;
                    print("Connected to embedded router - this.s" + i + " == mongod with pid " +
                          node.pid + " listening on routerPort " + node.routerPort);
                }
                if (i > 0) {
                    this.s = this._mongos[0];
                    this.admin = this._mongos[0].getDB('admin');
                    this.config = this._mongos[0].getDB('config');
                }
            } else {
                for (var i = 0; i < numMongos; i++) {
                    const options = mongosOptions[i];
                    options.configdb = this._configDB;

                    if (otherParams.useBridge) {
                        var bridgeOptions =
                            Object.merge(otherParams.bridgeOptions, options.bridgeOptions || {});
                        bridgeOptions = Object.merge(bridgeOptions, {
                            hostName: otherParams.useHostname ? hostName : "localhost",
                            port: _allocatePortForBridgeForMongos(),
                            // The mongos processes identify themselves to mongobridge as host:port,
                            // where the host is the actual hostname of the machine and not
                            // localhost.
                            dest: hostName + ":" + options.port,
                        });

                        var bridge = new MongoBridge(bridgeOptions);
                    }

                    var conn = MongoRunner.runMongos(options, clusterVersionInfo.isMixedVersion);
                    if (!conn) {
                        throw new Error("Failed to start mongos " + i);
                    }

                    if (otherParams.causallyConsistent) {
                        conn.setCausalConsistency(true);
                    }

                    if (otherParams.useBridge) {
                        bridge.connectToBridge();
                        this._mongos.push(bridge);
                        this._unbridgedMongos.push(conn);
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
            }

            _extendWithShMethods(this);

            // If auth is enabled for the test, login the mongos connections as system in order to
            // configure the instances and then log them out again.
            if (this.keyFile) {
                authutil.asCluster(this._mongos, this.keyFile, () => _configureCluster(this));
            } else if (mongosOptions[0] && mongosOptions[0].keyFile) {
                authutil.asCluster(
                    this._mongos, mongosOptions[0].keyFile, () => _configureCluster(this));
            } else {
                _configureCluster(this);
                // Ensure that all config server nodes are up to date with any changes made to
                // balancer settings before adding shards to the cluster. This prevents shards,
                // which read config.settings with readPreference 'nearest', from accidentally
                // fetching stale values from secondaries that aren't up-to-date.
                this.configRS.awaitLastOpCommitted();
            }

            try {
                if (!otherParams.manualAddShard) {
                    var testName = this._testName;
                    var admin = this.admin;
                    var keyFile = this.keyFile;

                    this._connections.forEach(function(z, idx) {
                        var n = z.name || z.host || z;

                        var name;
                        if (isConfigShardMode && idx == 0) {
                            name = "config";

                            if (!useAutoBootstrapProcedure) {
                                print("ShardingTest " + testName +
                                      " transitioning to config shard");

                                function transitionFromDedicatedConfigServer() {
                                    return assert.commandWorked(
                                        admin.runCommand({transitionFromDedicatedConfigServer: 1}));
                                }

                                if (keyFile) {
                                    authutil.asCluster(admin.getMongo(),
                                                       keyFile,
                                                       transitionFromDedicatedConfigServer);
                                } else if (mongosOptions[0] && mongosOptions[0].keyFile) {
                                    authutil.asCluster(admin.getMongo(),
                                                       mongosOptions[0].keyFile,
                                                       transitionFromDedicatedConfigServer);
                                } else {
                                    transitionFromDedicatedConfigServer();
                                }
                            }

                            z.shardName = name;
                        } else {
                            print("ShardingTest " + testName + " going to add shard : " + n);

                            let addShardCmd = {addShard: n};
                            if (alwaysUseTestNameForShardName) {
                                addShardCmd.name = `${testName}-${idx}`;
                            }

                            var result = assert.commandWorked(admin.runCommand(addShardCmd),
                                                              "Failed to add shard " + n);
                            z.shardName = result.shardAdded;
                        }
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
            this.configRS.getPrimary().getDB("admin").runCommand(
                {refreshLogicalSessionCacheNow: 1});

            // Ensure that all CSRS nodes are up to date. This is strictly needed for tests that use
            // multiple mongoses. In those cases, the first mongos initializes the contents of the
            // 'config' database, but without waiting for those writes to replicate to all the
            // config servers then the secondary mongoses risk reading from a stale config server
            // and seeing an empty config database.
            this.configRS.awaitLastOpCommitted();

            if (useAutoBootstrapProcedure) {
                // This is needed because auto-bootstrapping will initially create a config.shards
                // entry for the config shard where the host field does not contain all the nodes in
                // the replica set.
                assert.soonNoExcept(() => {
                    function getConfigShardDoc() {
                        return csrsPrimary.getDB("config").shards.findOne({_id: "config"});
                    }
                    const configShardDoc = this.keyFile
                        ? authutil.asCluster(csrsPrimary, this.keyFile, getConfigShardDoc)
                        : getConfigShardDoc();

                    // TODO SERVER-89498: This check is flaky and should be fixed before re-enabling
                    // the autobootstrap procedure. See BF-31879 for more details.
                    return configShardDoc.host == this.configRS.getURL();
                });
            }

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
                            conn.fullOptions.tlsCertificateKeyFile
                                ? conn.fullOptions.tlsCertificateKeyFile
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
                    const keyFileLocal = (otherParams.shards && otherParams.shards[i] &&
                                          otherParams.shards[i].keyFile)
                        ? otherParams.shards[i].keyFile
                        : this.keyFile;

                    const rs = this._rs[i].test;
                    flushRT(rs.getPrimary(), keyFileLocal);
                }

                this.waitForShardingInitialized();
            }
        } catch (e) {
            // this was expected to fail, so clean up appropriately
            if (params.shouldFailInit === true) {
                this.stopOnFail();
            }
            throw e;
        }
        // This initialization was expected to fail, but it did not.
        assert.neq(true,
                   params.shouldFailInit,
                   "This was expected to fail initialization, but it did not");
    }
}

// Stub for a hook to check that the cluster-wide metadata is consistent.
ShardingTest.prototype.checkMetadataConsistency = function() {
    print("Unhooked checkMetadataConsistency function");
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

ShardingTest.prototype.checkShardFilteringMetadata = function() {
    print("Unhooked checkShardFilteringMetadata function");
};

/**
 * Constructs a human-readable string representing a chunk's range.
 */
function _rangeToString(r) {
    return tojsononeline(r.min) + " -> " + tojsononeline(r.max);
}

/**
 * Extends the ShardingTest class with the methods exposed by the sh utility class.
 */
function _extendWithShMethods(st) {
    Object.keys(sh).forEach(function(fn) {
        if (typeof sh[fn] !== 'function') {
            return;
        }

        assert.eq(undefined,
                  st[fn],
                  'ShardingTest contains a method ' + fn +
                      ' which duplicates a method with the same name on sh. ' +
                      'Please select a different function name.');

        st[fn] = function() {
            const oldDb = globalThis.db;
            globalThis.db = st.getDB('test');

            try {
                return sh[fn].apply(sh, arguments);
            } finally {
                globalThis.db = oldDb;
            }
        };
    });
}

/**
 * Configures the cluster based on the specified parameters (balancer state, etc).
 */
function _configureCluster(st) {
    if (!st._otherParams.enableBalancer) {
        st.stopBalancer();
    }
}

function connectionURLTheSame(a, b) {
    if (a == b)
        return true;

    if (!a || !b)
        return false;

    if (a.name)
        return connectionURLTheSame(a.name, b);
    if (b.name)
        return connectionURLTheSame(a, b.name);

    if (a.host)
        return connectionURLTheSame(a.host, b);
    if (b.host)
        return connectionURLTheSame(a, b.host);

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

// These appear to be unit tests for `connectionURLTheSame`
assert(connectionURLTheSame("foo", "foo"));
assert(!connectionURLTheSame("foo", "bar"));
assert(connectionURLTheSame("foo/a,b", "foo/b,a"));
assert(!connectionURLTheSame("foo/a,b", "bar/a,b"));

/**
 * Returns boolean for whether the sharding test is compatible to shutdown in parallel.
 */
function isShutdownParallelSupported(st, opts = {}) {
    if (st._useBridge) {
        // Keep the current behavior of shutting down each replica set shard and the
        // CSRS individually when otherParams.useBridge === true. There appear to only
        // be 8 instances of {useBridge: true} with ShardingTest and the implementation
        // complexity is too high
        return false;
    }

    if (st._otherParams.configOptions && st._otherParams.configOptions.clusterAuthMode === "x509") {
        // The mongo shell performing X.509 authentication as a cluster member requires
        // starting a parallel shell and using the server's (not the client's)
        // certificate. The ReplSetTest instance constructed in a Thread wouldn't have
        // copied the path to the server's certificate. We therefore fall back to
        // initiating the CSRS and replica set shards sequentially when X.509
        // authentication is being used.
        return false;
    }

    if (st._otherParams.configOptions && st._otherParams.configOptions.tlsMode === "preferTLS") {
        return false;
    }

    if (st._otherParams.configOptions && st._otherParams.configOptions.sslMode === "requireSSL") {
        return false;
    }

    if (opts.parallelSupported !== undefined && opts.parallelSupported === false) {
        // The test has chosen to opt out of parallel shutdown
        return false;
    }

    return true;
}

/**
 * Returns the replica sets args for sets that are to be terminated in parallel threads.
 */
function replicaSetsToTerminate(st, shardRS) {
    const replicaSetsToTerminate = [];
    [...(shardRS.map(obj => obj.test))].forEach(rst => {
        // Generating a list of live nodes in the replica set
        const liveNodes = [];
        const pidValues = [];
        rst.nodes.forEach(function(node) {
            try {
                node.getDB('admin')._helloOrLegacyHello();
                liveNodes.push(node);
            } catch (err) {
                // Ignore since the node is not live
                print("ShardingTest replicaSetsToTerminate ignoring: " + node.host);
                return;
            }

            if (!node.pid) {
                // Getting the pid for the node
                let serverStatus;
                rst.keyFile = rst.keyFile ? rst.keyFile : st.keyFile;
                if (rst.keyFile) {
                    serverStatus = authutil.asCluster(node, rst.keyFile, () => {
                        return node.getDB("admin").serverStatus();
                    });
                } else {
                    serverStatus = node.getDB("admin").serverStatus();
                }

                if (serverStatus["pid"]) {
                    node.pid = serverStatus["pid"];
                } else {
                    // Shutdown requires PID values for every node. The case we are
                    // unable to obtain a PID value is rare, however, should it
                    // occur, the code will throw this error.
                    throw 'Could not obtain node PID value. Shutdown failed.';
                }
            }
            pidValues.push(node.pid.valueOf());
        });

        if (pidValues.length > 0) {
            // the number of livenodes must match the number of pidvalues being passed in
            // rst.Args to ensure the replica set is constructed correctly
            assert(liveNodes.length == pidValues.length);

            const hostName =
                st._otherParams.host === undefined ? getHostName() : st._otherParams.host;
            replicaSetsToTerminate.push({
                // Arguments for each replica set within parallel threads.
                rstArgs: {
                    name: rst.name,
                    nodeHosts: liveNodes.map(node => `${node.name}`),
                    nodeOptions: rst.nodeOptions,
                    // Mixed-mode SSL tests may specify a keyFile per replica set rather
                    // than one for the whole cluster.
                    keyFile: rst.keyFile ? rst.keyFile : st.keyFile,
                    host: st._otherParams.useHostname ? hostName : "localhost",
                    waitForKeys: false,
                    pidValue: pidValues
                },
            });
        }
    });
    return replicaSetsToTerminate;
}

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
function totalNumShardNodes(st) {
    const numNodesPerReplSet = st._rs.map(r => r.test.nodes.length);
    return numNodesPerReplSet.reduce((a, b) => a + b, 0);
}

function clusterHasBinVersion(st, version) {
    const binVersion = MongoRunner.getBinVersionFor(version);
    const hasBinVersionInParams = (params) => {
        return params && params.binVersion &&
            MongoRunner.areBinVersionsTheSame(binVersion,
                                              MongoRunner.getBinVersionFor(params.binVersion));
    };

    // Must check mongosBinVersion because it does not update mongosOptions.binVersion.
    const isMixedVersionMongos = jsTestOptions().mongosBinVersion &&
        MongoRunner.areBinVersionsTheSame(binVersion, jsTestOptions().mongosBinVersion);

    if (isMixedVersionMongos) {
        return true;
    }

    // Check for config servers.
    if (hasBinVersionInParams(st._otherParams.configOptions)) {
        return true;
    }

    const numConfigs = st._numConfigs;
    for (let i = 0; i < numConfigs; ++i) {
        if (hasBinVersionInParams(st._otherParams['c' + i])) {
            return true;
        }
    }

    // Check for mongod servers.
    if (hasBinVersionInParams(st._otherParams.shardOptions)) {
        return true;
    }

    if (hasBinVersionInParams(st._otherParams.rs)) {
        return true;
    }

    const numShards = st._numShards;
    for (let i = 0; i < numShards; ++i) {
        if (hasBinVersionInParams(st._otherParams['d' + i])) {
            return true;
        }
        if (hasBinVersionInParams(st._otherParams['rs' + i])) {
            return true;
        }
    }

    // Check for mongos servers.
    if (hasBinVersionInParams(st._otherParams.mongosOptions)) {
        return true;
    }

    const numMongos = st._numMongos;
    for (let i = 0; i < numMongos; ++i) {
        if (hasBinVersionInParams(st._otherParams['s' + i])) {
            return true;
        }
    }

    return false;
}

function defineReadOnlyProperty(st, name, value) {
    Object.defineProperty(
        st, name, {value: value, writable: false, enumerable: true, configurable: false});
}
