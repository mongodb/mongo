/**
 * Starts up a sharded cluster with the given specifications. The cluster
 * will be fully operational after the execution of this constructor function.
 * 
 * @param {Object} testName Contains the key value pair for the cluster
 *   configuration. Accepted keys are:
 * 
 *   {
 *     name {string}: name for this test
 *     verbose {number}: the verbosity for the mongos
 *     keyFile {string}: the location of the keyFile
 *     chunksize {number}:
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
 *         For other options, @see ReplSetTest#start
 *       }
 * 
 *     shards {number|Object|Array.<Object>}: number of shards or shard
 *       configuration object(s)(*). @see MongoRunner.runMongod
 *     
 *     config {number|Object|Array.<Object>}: number of config server or
 *       config server configuration object(s)(*). If this field has 3 or
 *       more members, it implies other.sync = true. @see MongoRunner.runMongod
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
 *     other: {
 *       nopreallocj: same as above
 *       rs: same as above
 *       chunksize: same as above
 *
 *       shardOptions {Object}: same as the shards property above.
 *          Can be used to specify options that are common all shards.
 * 
 *       sync {boolean}: Use SyncClusterConnection, and readies
 *          1 or 3 config servers, based on the value of numConfigs.
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
 *       // replica Set only:
 *       rsOptions {Object}: same as the rs property above. Can be used to
 *         specify options that are common all replica members.
 *       useHostname {boolean}: if true, use hostname of machine,
 *         otherwise use localhost
 *       numReplicas {number}
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

    // Used for counting the test duration
    var _startTime = new Date();

    // ShardingTest API

    this.getRSEntry = function(setName) {
        for (var i=0; i<this._rs.length; i++)
            if (this._rs[i].setName == setName)
                return this._rs[i];
        throw Error("can't find rs: " + setName);
    };

    this.getDB = function(name) {
        return this.s.getDB(name);
    };

    this.getServerName = function(dbname) {
        var x = this.config.databases.findOne({ _id : "" + dbname });
        if (x)
            return x.primary;
        this.config.databases.find().forEach(printjson);
        throw Error("couldn't find dbname: " + dbname + " total: " + this.config.databases.count());
    };

    this.getNonPrimaries = function(dbname) {
        var x = this.config.databases.findOne({ _id : dbname });
        if (! x) {
            this.config.databases.find().forEach(printjson);
            throw Error("couldn't find dbname: " + dbname + " total: " + this.config.databases.count());
        }

        return this.config.shards.find({ _id : { $ne : x.primary } }).map(function(z) { return z._id; })
    };

    this.getConnNames = function() {
        var names = [];
        for (var i=0; i<this._connections.length; i++) {
            names.push(this._connections[i].name);
        }
        return names;
    };

    this.getServer = function(dbname) {
        var name = this.getServerName(dbname);

        var x = this.config.shards.findOne({ _id : name });
        if (x)
            name = x.host;

        var rsName = null;
        if (name.indexOf("/") > 0)
        rsName = name.substring(0 , name.indexOf("/"));

        for (var i=0; i<this._connections.length; i++) {
            var c = this._connections[i];
            if (connectionURLTheSame(name , c.name) || 
                 connectionURLTheSame(rsName , c.name))
                return c;
        }

        throw Error("can't find server for: " + dbname + " name:" + name);
    };

    this.normalize = function(x) {
        var z = this.config.shards.findOne({ host : x });
        if (z)
            return z._id;
        return x;
    };

    this.getOther = function(one) {
        if (this._connections.length < 2)
            throw Error("getOther only works with 2 servers");

        if (one._mongo)
            one = one._mongo
        
        for(var i = 0; i < this._connections.length; i++) {
            if (this._connections[i] != one) return this._connections[i]
        }
        
        return null;
    };

    this.getAnother = function(one) {
        if (this._connections.length < 2)
            throw Error("getAnother() only works with multiple servers");
        
        if (one._mongo)
            one = one._mongo
        
        for(var i = 0; i < this._connections.length; i++) {
            if (this._connections[i] == one)
                return this._connections[(i + 1) % this._connections.length];
        }
    };

    this.getFirstOther = function(one) {
        for (var i=0; i<this._connections.length; i++) {
            if (this._connections[i] != one)
            return this._connections[i];
        }
        throw Error("impossible");
    };

    this.stop = function() {
        for (var i = 0; i < this._mongos.length; i++) {
            this.stopMongos(i);
        }

        for (var i = 0; i < this._connections.length; i++) {
            if (this._rs[i]) {
                this._rs[i].test.stopSet(15);
            } else {
                this.stopMongod(i);
            }
        }

        if (this.configRS) {
            this.configRS.stopSet();
        } else {
            // Old style config triplet
            for (var i = 0; i < this._configServers.length; i++) {
                if (otherParams.useBridge) {
                    MongoRunner.stopMongod(unbridgedConfigServers[i]);
                    this._configServers[i].stop();
                } else {
                    MongoRunner.stopMongod(this._configServers[i]);
                }
            }
        }

        for (var i = 0; i < this._alldbpaths.length; i++) {
            resetDbpath(MongoRunner.dataPath + this._alldbpaths[i]);
        }

        var timeMillis = new Date().getTime() - _startTime.getTime();

        print('*** ShardingTest ' + this._testName + " completed successfully in " + (timeMillis / 1000) + " seconds ***");
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
                msg += _rangeToString(z.details.before) + " -->> (" + _rangeToString(z.details.left) + "), (" + _rangeToString(z.details.right) + ")";
            }
            else if (z.what == "multi-split") {
                msg += _rangeToString(z.details.before) + "  -->> (" + z.details.number + "/" + z.details.of + " " + _rangeToString(z.details.chunk) + ")";
            }
            else {
                msg += tojsononeline(z.details);
            }

            print("ShardingTest " + msg);
        });
    };

    this.getChunksString = function(ns) {
        var q = {}
        if (ns)
            q.ns = ns;

        var s = "";
        this.config.chunks.find(q).sort({ ns : 1 , min : 1 }).forEach(
            function(z) {
                s +=  "  " + z._id + "\t" + z.lastmod.t + "|" + z.lastmod.i + "\t" + tojson(z.min) + " -> " + tojson(z.max) + " " + z.shard + "  " + z.ns + "\n";
            }
       );
        
        return s;
    };

    this.printChunks = function(ns) {
        print("ShardingTest " + this.getChunksString(ns));
    };

    this.printShardingStatus = function() {
        printShardingStatus(this.config);
    };

    this.printCollectionInfo = function(ns , msg) {
        var out = "";
        if (msg)
            out += msg + "\n";
        out += "sharding collection info: " + ns + "\n";
        for (var i=0; i<this._connections.length; i++) {
            var c = this._connections[i];
            out += "  mongod " + c + " " + tojson(c.getCollection(ns).getShardVersion() , " " , true) + "\n";
        }
        for (var i=0; i<this._mongos.length; i++) {
            var c = this._mongos[i];
            out += "  mongos " + c + " " + tojson(c.getCollection(ns).getShardVersion() , " " , true) + "\n";
        }
        
        out += this.getChunksString(ns);

        print("ShardingTest " + out);
    };

    this.sync = function() {
        this.adminCommand("connpoolsync");
    };

    this.onNumShards = function(collName , dbName) {
        this.sync(); // we should sync since we're going directly to mongod here
        dbName = dbName || "test";
        var num=0;
        for (var i=0; i<this._connections.length; i++)
            if (this._connections[i].getDB(dbName).getCollection(collName).count() > 0)
                num++;
        return num;
    };

    this.shardCounts = function(collName , dbName) {
        this.sync(); // we should sync since we're going directly to mongod here
        dbName = dbName || "test";
        var counts = {}
        for (var i=0; i<this._connections.length; i++)
            counts[i] = this._connections[i].getDB(dbName).getCollection(collName).count();
        return counts;
    };

    this.chunkCounts = function(collName , dbName) {
        dbName = dbName || "test";
        var x = {}

        this.config.shards.find().forEach(
            function(z) {
                x[z._id] = 0;
            }
       );
        
        this.config.chunks.find({ ns : dbName + "." + collName }).forEach(
            function(z) {
                if (x[z.shard])
                    x[z.shard]++
                else
                    x[z.shard] = 1;
            }
       );

        return x;
    };

    this.chunkDiff = function(collName , dbName) {
        var c = this.chunkCounts(collName , dbName);
        var min = 100000000;
        var max = 0;
        for (var s in c) {
            if (c[s] < min)
                min = c[s];
            if (c[s] > max)
                max = c[s];
        }
        print("ShardingTest input: " + tojson(c) + " min: " + min + " max: " + max );
        return max - min;
    };

    // Waits up to one minute for the difference in chunks between the most loaded shard and least
    // loaded shard to be 0 or 1, indicating that the collection is well balanced.
    // This should only be called after creating a big enough chunk difference to trigger balancing.
    this.awaitBalance = function(collName , dbName , timeToWait) {
        timeToWait = timeToWait || 60000;
        var shardingTest = this;
        assert.soon(function() {
            var x = shardingTest.chunkDiff(collName , dbName);
            print("chunk diff: " + x);
            return x < 2;
        } , "no balance happened", 60000);
    };

    this.getShardNames = function() {
        var shards = [];
        this.s.getCollection("config.shards").find().forEach(function(shardDoc) {
                                                                 shards.push(shardDoc._id);
                                                             });
        return shards;
    };

    this.getShard = function(coll, query, includeEmpty) {
        var shards = this.getShardsForQuery(coll, query, includeEmpty)
        assert.eq(shards.length, 1)
        return shards[0]
    };

    /**
     * Returns the shards on which documents matching a particular query reside.
     */
    this.getShardsForQuery = function(coll, query, includeEmpty) {
        if (! coll.getDB)
            coll = this.s.getCollection(coll)

        var explain = coll.find(query).explain("executionStats")
        var shards = []

        var execStages = explain.executionStats.executionStages;
        var plannerShards = explain.queryPlanner.winningPlan.shards;

        if (execStages.shards) {
            for(var i = 0; i < execStages.shards.length; i++) {
                var hasResults = execStages.shards[i].executionStages.nReturned &&
                                 execStages.shards[i].executionStages.nReturned > 0;
                if (includeEmpty || hasResults) {
                    shards.push(plannerShards[i].connectionString);
                }
            }
        }

        for(var i = 0; i < shards.length; i++) {
            for(var j = 0; j < this._connections.length; j++) {
                if (connectionURLTheSame( this._connections[j] , shards[i])) {
                    shards[i] = this._connections[j]
                    break;
                }
            }
        }

        return shards;
    };

    this.isSharded = function(collName) {
        var collName = "" + collName
        var dbName = undefined
        
        if (typeof collName.getCollectionNames == 'function') {
            dbName = "" + collName
            collName = undefined
        }

        if (dbName) {
            var x = this.config.databases.findOne({ _id : dbname })
            if (x) return x.partitioned
            else return false
        }

        if (collName) {
            var x = this.config.collections.findOne({ _id : collName })
            if (x) return true
            else return false
        }    
    };

    this.shardColl = function(collName , key , split , move , dbName, waitForDelete) {
        split = (split != false ? (split || key) : split)
        move = (split != false && move != false ? (move || split) : false)

        if (collName.getDB)
            dbName = "" + collName.getDB()
        else 
            dbName = dbName || "test";

        var c = dbName + "." + collName;
        if (collName.getDB)
            c = "" + collName

        var isEmpty = (this.s.getCollection(c).count() == 0);

        if (! this.isSharded(dbName))
            this.s.adminCommand({ enableSharding : dbName })

        var result = this.s.adminCommand({ shardcollection : c , key : key })
        if (! result.ok) {
            printjson(result)
            assert(false)
        }

        if (split == false) return;

        result = this.s.adminCommand({ split : c , middle : split });
        if (! result.ok) {
            printjson(result)
            assert(false)
        }

        if (move == false) return;

        var result = null
        for(var i = 0; i < 5; i++) {
            result = this.s.adminCommand({ movechunk : c , find : move , to : this.getOther(this.getServer(dbName)).name, _waitForDelete: waitForDelete });
            if (result.ok) break;
            sleep(5 * 1000);
        }

        printjson(result);
        assert(result.ok);
    };

    this.stopBalancer = function(timeout, interval) {
        if (typeof db == "undefined") db = undefined;
        var oldDB = db;
        db = this.config;

        try {
            sh.setBalancerState(false);
            sh.waitForBalancer(false, timeout, interval);
        }
        finally {
            db = oldDB;
        }
    };

    this.startBalancer = function(timeout, interval) {
        if (typeof db == "undefined") db = undefined;
        var oldDB = db;
        db = this.config;

        try {
            sh.setBalancerState(true);
            sh.waitForBalancer(true, timeout, interval);
        }
        finally {
            db = oldDB;
        }
    };

    this.isAnyBalanceInFlight = function() {
        if (this.config.locks.find({ _id : { $ne : "balancer" }, state : 2 }).count() > 0)
            return true;

        var allCurrent = this.s.getDB("admin").currentOp().inprog;
        for (var i = 0; i < allCurrent.length; i++) {
            if (allCurrent[i].desc &&
                 allCurrent[i].desc.indexOf("cleanupOldData") == 0)
                return true;
        }
        return false;
    };

    /**
     * Kills the mongos with index n.
     */
    this.stopMongos = function(n) {
        if (otherParams.useBridge) {
            MongoRunner.stopMongos(unbridgedMongos[n]);
            this["s" + n].stop();
        } else {
            MongoRunner.stopMongos(this["s" + n]);
        }
    };

    /**
     * Kills the mongod with index n.
     */
    this.stopMongod = function(n) {
        if (otherParams.useBridge) {
            MongoRunner.stopMongod(unbridgedConnections[n]);
            this["d" + n].stop();
        } else {
            MongoRunner.stopMongod(this["d" + n]);
        }
    };

    /**
     * Restarts a previously stopped mongos.
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
            this._mongos[n] = new MongoBridge({
                hostName: otherParams.useHostname ? hostName : "localhost",
                port: this._mongos[n].port,
                // The mongos processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + opts.port,
            });
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
     * Restarts a previously stopped mongod using the same parameters as before.
     *
     * Warning: Overwrites the old dn member variables.
     */
    this.restartMongod = function(n) {
        var mongod;

        if (otherParams.useBridge) {
            mongod = unbridgedConnections[n];
        } else {
            mongod = this["d" + n];
        }

        this.stopMongod(n);

        if (otherParams.useBridge) {
            this._connections[n] = new MongoBridge({
                hostName: otherParams.useHostname ? hostName : "localhost",
                port: this._connections[n].port,
                // The mongod processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + mongod.port,
            });
        }

        mongod.restart = true;
        var newConn = MongoRunner.runMongod(mongod);
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
     * Helper method for setting primary shard of a database and making sure that it was successful.
     * Note: first mongos needs to be up.
     */
    this.ensurePrimaryShard = function(dbName, shardName) {
        var db = this.s0.getDB('admin');
        var res = db.adminCommand({ movePrimary: dbName, to: shardName });
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

    // Allow specifying options like :
    // { mongos : [ { noprealloc : "" } ], config : [ { smallfiles : "" } ], shards : { rs : true, d : true } } 
    if (Array.isArray(numShards)) {
        for(var i = 0; i < numShards.length; i++) {
            otherParams[ "d" + i ] = numShards[i];
        }

        numShards = numShards.length;
    }
    else if (isObject(numShards)) {
        var tempCount = 0;
        for(var i in numShards) {
            otherParams[ i ] = numShards[i];
            tempCount++;
        }

        numShards = tempCount;
    }

    if (Array.isArray(numMongos)) {
        for(var i = 0; i < numMongos.length; i++) {
            otherParams[ "s" + i ] = numMongos[i];
        }

        numMongos = numMongos.length;
    }
    else if (isObject(numMongos)) {
        var tempCount = 0;
        for(var i in numMongos) {
            otherParams[ i ] = numMongos[i];
            tempCount++;
        }

        numMongos = tempCount;
    }

    if (Array.isArray(numConfigs)) {
        for(var i = 0; i < numConfigs.length; i++) {
            otherParams[ "c" + i ] = numConfigs[i];
        }

        numConfigs = numConfigs.length
    }
    else if (isObject(numConfigs)) {
        var tempCount = 0;
        for(var i in numConfigs) {
            otherParams[ i ] = numConfigs[i];
            tempCount++;
        }

        numConfigs = tempCount;
    }

    otherParams.extraOptions = otherParams.extraOptions || {};
    otherParams.useHostname = otherParams.useHostname == undefined ?
        true : otherParams.useHostname;
    otherParams.useBridge = otherParams.useBridge || false;
    var keyFile = otherParams.keyFile || otherParams.extraOptions.keyFile
    var hostName = getHostName();

    this._testName = testName
    this._otherParams = otherParams
    
    var pathOpts = this.pathOpts = { testName : testName }

    var hasRS = false
    for(var k in otherParams) {
        if (k.startsWith("rs") && otherParams[k] != undefined) {
            hasRS = true
            break
        }
    }

    this._alldbpaths = []
    this._connections = []
    this._shardServers = this._connections
    this._rs = []
    this._rsObjects = []

    if (otherParams.useBridge) {
        var unbridgedConnections = [];
        var unbridgedConfigServers = [];
        var unbridgedMongos = [];
    }

    // Start the MongoD servers (shards)
    for (var i = 0; i < numShards; i++) {
        if (otherParams.rs || otherParams["rs" + i]) {
            var setName = testName + "-rs" + i;

            rsDefaults = { useHostname : otherParams.useHostname,
                           noJournalPrealloc : otherParams.nopreallocj, 
                           oplogSize : 16,
                           pathOpts : Object.merge(pathOpts, { shard : i })}

            rsDefaults = Object.merge(rsDefaults, ShardingTest.rsOptions || {})
            rsDefaults = Object.merge(rsDefaults, otherParams.rs)
            rsDefaults = Object.merge(rsDefaults, otherParams.rsOptions)
            rsDefaults = Object.merge(rsDefaults, otherParams["rs" + i])
            rsDefaults.nodes = rsDefaults.nodes || otherParams.numReplicas

            var numReplicas = rsDefaults.nodes || 3;
            delete rsDefaults.nodes;
            var protocolVersion = rsDefaults.protocolVersion;
            delete rsDefaults.protocolVersion;

            print("Replica set test!")

            var rs = new ReplSetTest({ name : setName,
                                       nodes : numReplicas,
                                       useHostName : otherParams.useHostname,
                                       useBridge: otherParams.useBridge,
                                       keyFile : keyFile,
                                       protocolVersion: protocolVersion,
                                       shardSvr : true });

            this._rs[i] = { setName : setName,
                            test : rs,
                            nodes : rs.startSet(rsDefaults),
                            url : rs.getURL() };

            rs.initiate();
            this["rs" + i] = rs

            this._rsObjects[i] = rs

            this._alldbpaths.push(null);
            this._connections.push(null);

            if (otherParams.useBridge) {
                unbridgedConnections.push(null);
            }
        }
        else {
            var options = {
                useHostname: otherParams.useHostname,
                noJournalPrealloc: otherParams.nopreallocj,
                pathOpts: Object.merge(pathOpts, {shard: i}),
                dbpath: "$testName$shard",
                keyFile: keyFile
            };

            options = Object.merge(options, ShardingTest.shardOptions || {})

            if (otherParams.shardOptions && otherParams.shardOptions.binVersion) {
                otherParams.shardOptions.binVersion =
                    MongoRunner.versionIterator(otherParams.shardOptions.binVersion)
            }

            options = Object.merge(options, otherParams.shardOptions)
            options = Object.merge(options, otherParams["d" + i])

            options.port = options.port || allocatePort();

            if (otherParams.useBridge) {
                var bridge = new MongoBridge({
                    hostName: otherParams.useHostname ? hostName : "localhost",
                    // The mongod processes identify themselves to mongobridge as host:port, where
                    // the host is the actual hostname of the machine and not localhost.
                    dest: hostName + ":" + options.port,
                });
            }

            var conn = MongoRunner.runMongod(options);
            if (!conn) {
                throw new Error("Failed to start shard " + i);
            }

            if (otherParams.useBridge) {
                bridge.connectToBridge();
                this._connections.push(bridge);
                unbridgedConnections.push(conn)
            } else {
                this._connections.push(conn);
            }

            this._alldbpaths.push(testName + i);
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
        
        rs.getMaster().getDB("admin").foo.save({ x : 1 })
        if (keyFile) {
            authutil.asCluster(rs.nodes, keyFile, function() { rs.awaitReplication(); });
        }
        rs.awaitSecondaryNodes();
        
        var rsConn = new Mongo(rs.getURL());
        rsConn.name = rs.getURL();
        this._connections[i] = rsConn
        this["shard" + i] = rsConn
        rsConn.rs = rs
    }

    // Default to using 3-node legacy config servers if jsTestOptions().useLegacyOptions is true
    // and the user didn't explicity specify a different config server configuration
    if (jsTestOptions().useLegacyConfigServers &&
            otherParams.sync !== false &&
            (typeof(otherParams.config) === 'undefined' || numConfigs === 3)) {
        otherParams.sync = true;
    }

    this._configServers = [];

    // Start the config servers
    if (otherParams.sync) {
        if (numConfigs !== 1 && numConfigs !== 3) {
            throw Error('Sync config servers only supported with 1 or 3 nodes');
        }

        var configNames = [];
        for (var i = 0; i < numConfigs; i++) {
            var options = { useHostname : otherParams.useHostname,
                            noJournalPrealloc : otherParams.nopreallocj,
                            pathOpts : Object.merge(pathOpts, { config : i }),
                            dbpath : "$testName-config$config",
                            keyFile : keyFile,
                            // Ensure that journaling is always enabled for config servers.
                            journal : "",
                            configsvr : "" };

            options = Object.merge(options, ShardingTest.configOptions || {})

            if (otherParams.configOptions && otherParams.configOptions.binVersion) {
                otherParams.configOptions.binVersion =
                    MongoRunner.versionIterator(otherParams.configOptions.binVersion)
            }

            options = Object.merge(options, otherParams.configOptions)
            options = Object.merge(options, otherParams["c" + i])

            options.port = options.port || allocatePort();

            if (otherParams.useBridge) {
                var bridge = new MongoBridge({
                    hostName: otherParams.useHostname ? hostName : "localhost",
                    // The mongod processes identify themselves to mongobridge as host:port, where
                    // the host is the actual hostname of the machine and not localhost.
                    dest: hostName + ":" + options.port,
                });
            }

            var conn = MongoRunner.runMongod(options);
            if (!conn) {
                throw new Error("Failed to start config server " + i);
            }

            if (otherParams.useBridge) {
                bridge.connectToBridge();
                this._configServers.push(bridge);
                unbridgedConfigServers.push(conn);
                configNames.push(bridge.host);
            } else {
                this._configServers.push(conn);
                configNames.push(conn.name);
            }

            this._alldbpaths.push(testName + "-config" + i);
            this["config" + i] = this._configServers[i];
            this["c" + i] = this._configServers[i];
        }

        this._configDB = configNames.join(',');
    }
    else {
        // Using replica set for config servers
        var rstOptions = { useHostName : otherParams.useHostname,
                           useBridge : otherParams.useBridge,
                           keyFile : keyFile,
                           name: testName + "-configRS",
                         };

        // when using CSRS, always use wiredTiger as the storage engine
        var startOptions = { pathOpts: pathOpts,
                             // Ensure that journaling is always enabled for config servers.
                             journal : "",
                             configsvr : "",
                             noJournalPrealloc : otherParams.nopreallocj,
                             storageEngine : "wiredTiger",
                           };

        startOptions = Object.merge(startOptions, ShardingTest.configOptions || {})

        if (otherParams.configOptions && otherParams.configOptions.binVersion) {
            otherParams.configOptions.binVersion =
                MongoRunner.versionIterator(otherParams.configOptions.binVersion)
        }

        startOptions = Object.merge(startOptions, otherParams.configOptions)
        var nodeOptions = [];
        for (var i = 0; i < numConfigs; ++i) {
            nodeOptions.push(otherParams["c" + i] || {});
        }
        rstOptions["nodes"] = nodeOptions;

        this.configRS = new ReplSetTest(rstOptions);
        this.configRS.startSet(startOptions);

        var config = this.configRS.getReplSetConfig();
        config.configsvr = true;
        config.settings = config.settings || {};
        this.configRS.initiate(config);

        this.configRS.getMaster(); // Wait for master to be elected before starting mongos

        this._configDB = this.configRS.getURL();
        this._configServers = this.configRS.nodes;
        for (var i = 0; i < numConfigs; ++i) {
            var conn = this._configServers[i];
            this["config" + i] = conn;
            this["c" + i] = conn;
        }
    }

    printjson("config servers: " + this._configDB);

    this._configConnection = _connectWithRetry(this._configDB);

    print("ShardingTest " + this._testName + " :\n" + tojson({ config : this._configDB, shards : this._connections }));

    if (numMongos == 0 && !otherParams.noChunkSize) {
        if (keyFile) {
            throw Error("Cannot set chunk size without any mongos when using auth");
        } else {
            this._configConnection.getDB("config").settings.insert(
                { _id : "chunksize" , value : otherParams.chunksize || otherParams.chunkSize || 50 });
        }
    }

    this._mongos = []

    // Start the MongoS servers
    for (var i = 0; i < ((numMongos == 0 ? -1 : numMongos) || 1); i++) {
        options = {
            useHostname: otherParams.useHostname,
            pathOpts: Object.merge(pathOpts, {mongos: i}),
            configdb: this._configDB,
            verbose: verboseLevel || 0,
            keyFile: keyFile
        };

        if (!otherParams.noChunkSize) {
            options.chunkSize = otherParams.chunksize || otherParams.chunkSize || 50;
        }

        options = Object.merge(options, ShardingTest.mongosOptions || {})

        if (otherParams.mongosOptions && otherParams.mongosOptions.binVersion) {
            otherParams.mongosOptions.binVersion =
                MongoRunner.versionIterator(otherParams.mongosOptions.binVersion);
        }

        options = Object.merge(options, otherParams.mongosOptions)
        options = Object.merge(options, otherParams.extraOptions)
        options = Object.merge(options, otherParams["s" + i])

        options.port = options.port || allocatePort();

        if (otherParams.useBridge) {
            var bridge = new MongoBridge({
                hostName: otherParams.useHostname ? hostName : "localhost",
                // The mongos processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: hostName + ":" + options.port,
            });
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

    // Disable the balancer unless it is explicitly turned on
    if (!otherParams.enableBalancer) {
        if (keyFile) {
            authutil.assertAuthenticate(this._mongos, 'admin', {
                user: '__system',
                mechanism: 'MONGODB-CR',
                pwd: cat(keyFile).replace(/[\011-\015\040]/g, '')
            });

            try {
                this.stopBalancer();
            }
            finally {
                authutil.logout(this._mongos, 'admin');
            }
        }
        else {
            this.stopBalancer();
        }
    }

    if (!otherParams.manualAddShard) {
        this._shardNames = [];

        var testName = this._testName;
        var admin = this.admin;
        var shardNames = this._shardNames;

        this._connections.forEach(
            function(z) {
                var n = z.name;
                if (!n) {
                    n = z.host;
                    if (!n) {
                        n = z;
                    }
                }

                print("ShardingTest " + testName + " going to add shard : " + n);

                var result = admin.runCommand({ addshard: n });
                assert.commandWorked(result, "Failed to add shard " + n);

                shardNames.push(result.shardAdded);
                z.shardName = result.shardAdded;
            }
       );
    }

    if (jsTestOptions().keyFile) {
        jsTest.authenticate(this._configConnection);
        jsTest.authenticateNodes(this._configServers);
        jsTest.authenticateNodes(this._mongos);
    }
};
