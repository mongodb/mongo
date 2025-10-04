/* global ShardingTest */

let sh = function () {
    return "try sh.help();";
};

sh._checkMongos = function () {
    if (TestData !== undefined && TestData.testingReplicaSetEndpoint) {
        // When testing the replica set endpoint, the test connects directly to a mongod on the
        // config shard which returns mongod hello responses (i.e. do not have "isdbgrid").
        return;
    }
    let x = globalThis.db._helloOrLegacyHello();
    if (x.msg != "isdbgrid") {
        throw Error("not connected to a mongos");
    }
};

sh._checkFullName = function (fullName) {
    assert(fullName, "need a full name");
    assert(fullName.indexOf(".") > 0, "name needs to be fully qualified <db>.<collection>'");
};

sh._adminCommand = function (cmd, skipCheck) {
    if (!skipCheck) sh._checkMongos();
    return globalThis.db.getSiblingDB("admin").runCommand(cmd);
};

sh._getConfigDB = function () {
    sh._checkMongos();
    return globalThis.db.getSiblingDB("config");
};

sh._getBalancerStatus = function () {
    return assert.commandWorked(sh._getConfigDB().adminCommand({balancerStatus: 1}));
};

sh._dataFormat = function (bytes) {
    if (bytes == null) {
        return "0B";
    }

    if (bytes < 1024) return Math.floor(bytes) + "B";
    if (bytes < 1024 * 1024) return Math.floor(bytes / 1024) + "KiB";
    if (bytes < 1024 * 1024 * 1024) return Math.floor((Math.floor(bytes / 1024) / 1024) * 100) / 100 + "MiB";
    return Math.floor((Math.floor(bytes / (1024 * 1024)) / 1024) * 100) / 100 + "GiB";
};

sh._collRE = function (coll) {
    return RegExp("^" + RegExp.escape(coll + "") + "-.*");
};

sh._pchunk = function (chunk) {
    return "[" + tojson(chunk.min) + " -> " + tojson(chunk.max) + "]";
};

/**
 * Internal method to write the balancer state to the config.settings collection. Should not be used
 * directly, instead go through the start/stopBalancer calls and the balancerStart/Stop commands.
 */
sh._writeBalancerStateDeprecated = function (onOrNot) {
    return assert.commandWorked(
        sh
            ._getConfigDB()
            .settings.update(
                {_id: "balancer"},
                {$set: {stopped: !onOrNot}},
                {upsert: true, writeConcern: {w: "majority"}},
            ),
    );
};

/**
 * Asserts the specified command is executed successfully. However, if a retryable error occurs, the
 * command is retried.
 */
sh.assertRetryableCommandWorkedOrFailedWithCodes = function (cmd, msg, expectedErrorCodes = []) {
    let res = undefined;
    assert.soon(function () {
        try {
            res = assert.commandWorked(
                (() => {
                    try {
                        return cmd();
                    } catch (err) {
                        return err;
                    }
                })(),
            );
            return true;
        } catch (err) {
            const errorCode = (() => {
                if (err.hasOwnProperty("code")) {
                    return err.code;
                }
                if (err.hasOwnProperty("writeErrors")) {
                    return err.writeErrors[err.writeErrors.length - 1].code;
                }
                if (err.hasOwnProperty("writeConcernError")) {
                    return err.writeConcernError.code;
                }
                return undefined;
            })();

            if (expectedErrorCodes.includes(errorCode)) {
                return true;
            }

            if (ErrorCodes.isRetriableError(errorCode)) {
                return false;
            }
            throw err;
        }
    }, msg);

    return res;
};

sh.help = function () {
    print("\tsh.addShard( host )                       server:port OR setname/server:port");
    print("\tsh.addShardToZone(shard,zone)             adds the shard to the zone");
    print(
        "\tsh.updateZoneKeyRange(fullName,min,max,zone)      " +
            "assigns the specified range of the given collection to a zone",
    );
    print("\tsh.disableBalancing(coll)                 disable balancing on one collection");
    print("\tsh.enableBalancing(coll)                  re-enable balancing on one collection");
    print(
        "\tsh.enableSharding(dbname, shardName)      enables sharding on the database dbname, optionally use shardName as primary",
    );
    print("\tsh.getBalancerState()                     returns whether the balancer is enabled");
    print("\tsh.isBalancerRunning()                    return true if the balancer has work in progress on any mongos");
    print("\tsh.moveChunk(fullName,find,to)            move the chunk where 'find' is to 'to' (name of shard)");
    print("\tsh.removeShardFromZone(shard,zone)      removes the shard from zone");
    print("\tsh.removeRangeFromZone(fullName,min,max)   removes the range of the given collection from any zone");
    print("\tsh.shardCollection(fullName,key,unique,options)   shards the collection");
    print("\tsh.splitAt(fullName,middle)               splits the chunk that middle is in at middle");
    print("\tsh.splitFind(fullName,find)               splits the chunk that find is in at the median");
    print("\tsh.startBalancer()                        starts the balancer so chunks are balanced automatically");
    print("\tsh.status()                               prints a general overview of the cluster");
    print("\tsh.stopBalancer()                         stops the balancer so chunks are not balanced automatically");
    print("\tsh.startAutoMerger()                      globally enable auto-merger (active only if balancer is up)");
    print("\tsh.stopAutoMerger()                     globally disable auto-merger");
    print("\tsh.shouldAutoMerge()                    returns whether the auto-merger is enabled");
    print("\tsh.disableAutoMerger(coll)              disable auto-merging on one collection");
    print("\tsh.enableAutoMerger(coll)               re-enable auto-merge on one collection");
    print(
        "\tsh.balancerCollectionStatus(fullName)       " +
            "returns wheter the specified collection is balanced or the balancer needs to take more actions on it",
    );
    print(
        "\tsh.configureCollectionBalancing(fullName, params)       " +
            "configure balancing settings for a specific collection",
    );
    print("\tsh.awaitCollectionBalance(coll)         waits for a collection to be balanced");
    print(
        "\tsh.verifyCollectionIsBalanced(coll)       verifies that a collection is well balanced by checking the actual data size on each shard",
    );
};

sh.status = function (verbose, configDB) {
    // TODO: move the actual command here
    printShardingStatus(configDB, verbose);
};

sh.addShard = function (url) {
    return sh._adminCommand({addShard: url}, true);
};

sh.enableSharding = function (dbname, shardName) {
    assert(dbname, "need a valid dbname");
    let command = {enableSharding: dbname};
    if (shardName) {
        command.primaryShard = shardName;
    }
    return sh._adminCommand(command);
};

sh.shardCollection = function (fullName, key, unique, options) {
    sh._checkFullName(fullName);
    assert(key, "need a key");
    assert(typeof key == "object", "key needs to be an object");

    let cmd = {shardCollection: fullName, key};
    if (unique) cmd.unique = true;
    if (options) {
        if (typeof options !== "object") {
            throw new Error("options must be an object");
        }
        Object.extend(cmd, options);
    }

    return sh._adminCommand(cmd);
};

sh.splitFind = function (fullName, find) {
    sh._checkFullName(fullName);
    return sh._adminCommand({split: fullName, find});
};

sh.splitAt = function (fullName, middle) {
    sh._checkFullName(fullName);
    return sh._adminCommand({split: fullName, middle});
};

sh.moveChunk = function (fullName, find, to) {
    sh._checkFullName(fullName);
    return sh._adminCommand({moveChunk: fullName, find, to});
};

sh.setBalancerState = function (isOn) {
    assert(typeof isOn == "boolean", "Must pass boolean to setBalancerState");
    if (isOn) {
        return sh.startBalancer();
    } else {
        return sh.stopBalancer();
    }
};

sh.getBalancerState = function (configDB, balancerStatus) {
    if (configDB === undefined) {
        configDB = sh._getConfigDB();
    }

    if (balancerStatus === undefined) {
        balancerStatus = assert.commandWorked(configDB.adminCommand({balancerStatus: 1}));
    }

    return balancerStatus.mode !== "off";
};

sh.isBalancerRunning = function (configDB) {
    if (configDB === undefined) configDB = sh._getConfigDB();

    let result = configDB.adminCommand({balancerStatus: 1});
    return assert.commandWorked(result).inBalancerRound;
};

sh.stopBalancer = function (timeoutMs, interval) {
    timeoutMs ||= 60000;

    let result = globalThis.db.adminCommand({balancerStop: 1, maxTimeMS: timeoutMs});
    return assert.commandWorked(result);
};

sh.startBalancer = function (timeoutMs, interval) {
    timeoutMs ||= 60000;

    let result = globalThis.db.adminCommand({balancerStart: 1, maxTimeMS: timeoutMs});
    return assert.commandWorked(result);
};

sh.startAutoMerger = function (configDB) {
    if (configDB === undefined) configDB = sh._getConfigDB();

    // Set retryable write since mongos doesn't do it automatically.
    const mongosSession = configDB.getMongo().startSession({retryWrites: true});
    const sessionConfigDB = mongosSession.getDatabase("config");
    return assert.commandWorked(
        sessionConfigDB.settings.update(
            {_id: "automerge"},
            {$set: {enabled: true}},
            {upsert: true, writeConcern: {w: "majority", wtimeout: 30000}},
        ),
    );
};

sh.stopAutoMerger = function (configDB) {
    if (configDB === undefined) configDB = sh._getConfigDB();

    // Set retryable write since mongos doesn't do it automatically.
    const mongosSession = configDB.getMongo().startSession({retryWrites: true});
    const sessionConfigDB = mongosSession.getDatabase("config");
    return assert.commandWorked(
        sessionConfigDB.settings.update(
            {_id: "automerge"},
            {$set: {enabled: false}},
            {upsert: true, writeConcern: {w: "majority", wtimeout: 30000}},
        ),
    );
};

sh.shouldAutoMerge = function (configDB) {
    if (configDB === undefined) configDB = sh._getConfigDB();
    let automerge = configDB.settings.findOne({_id: "automerge"});
    if (automerge == null) {
        return true;
    }
    return automerge.enabled;
};

sh.disableAutoMerger = function (coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    let dbase = globalThis.db;
    if (coll instanceof DBCollection) {
        dbase = coll.getDB();
    } else {
        sh._checkMongos();
    }

    return dbase.adminCommand({configureCollectionBalancing: coll + "", enableAutoMerger: false});
};

sh.enableAutoMerger = function (coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    let dbase = globalThis.db;
    if (coll instanceof DBCollection) {
        dbase = coll.getDB();
    } else {
        sh._checkMongos();
    }

    return dbase.adminCommand({configureCollectionBalancing: coll + "", enableAutoMerger: true});
};

sh.waitForPingChange = function (activePings, timeout, interval) {
    let isPingChanged = function (activePing) {
        let newPing = sh._getConfigDB().mongos.findOne({_id: activePing._id});
        return !newPing || newPing.ping + "" != activePing.ping + "";
    };

    // First wait for all active pings to change, so we're sure a settings reload
    // happened

    // Timeout all pings on the same clock
    let start = new Date();

    let remainingPings = [];
    for (let i = 0; i < activePings.length; i++) {
        let activePing = activePings[i];
        print(
            "Waiting for active host " +
                activePing._id +
                " to recognize new settings... (ping : " +
                activePing.ping +
                ")",
        );

        // Do a manual timeout here, avoid scary assert.soon errors
        timeout ||= 30000;
        interval ||= 200;
        while (isPingChanged(activePing) != true) {
            if (new Date().getTime() - start.getTime() > timeout) {
                print(
                    "Waited for active ping to change for host " +
                        activePing._id +
                        ", a migration may be in progress or the host may be down.",
                );
                remainingPings.push(activePing);
                break;
            }
            sleep(interval);
        }
    }

    return remainingPings;
};

/**
 * Waits up to the specified timeout (with a default of 60s) for the balancer to execute one
 * round. If no round has been executed, throws an error.
 */
sh.awaitBalancerRound = function (timeout, interval) {
    timeout ||= 60000;

    let initialStatus = sh._getBalancerStatus();
    let currentStatus;
    assert.soon(
        function () {
            currentStatus = sh._getBalancerStatus();
            assert.eq(currentStatus.mode, "full", "Balancer is disabled");
            if (!friendlyEqual(currentStatus.term, initialStatus.term)) {
                // A new primary of the csrs has been elected
                initialStatus = currentStatus;
                return false;
            }
            assert.gte(
                currentStatus.numBalancerRounds,
                initialStatus.numBalancerRounds,
                "Number of balancer rounds moved back in time unexpectedly. Current status: " +
                    tojson(currentStatus) +
                    ", initial status: " +
                    tojson(initialStatus),
            );
            return currentStatus.numBalancerRounds > initialStatus.numBalancerRounds;
        },
        "Latest balancer status: " + tojson(currentStatus),
        timeout,
        interval,
    );
};

sh.disableBalancing = function (coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    if (!(coll instanceof DBCollection)) {
        throw Error("Collection must be a DBCollection");
    }
    return coll.disableBalancing();
};

sh.enableBalancing = function (coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    if (!(coll instanceof DBCollection)) {
        throw Error("Collection must be a DBCollection");
    }
    return coll.enableBalancing();
};

sh.awaitCollectionBalance = function (coll, timeout, interval) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    timeout ||= 60000;
    interval ||= 200;

    const ns = coll.getFullName();
    const orphanDocsPipeline = [
        {"$collStats": {"storageStats": {}}},
        {"$project": {"shard": true, "storageStats": {"numOrphanDocs": true}}},
        {"$group": {"_id": null, "totalNumOrphanDocs": {"$sum": "$storageStats.numOrphanDocs"}}},
    ];

    let oldDb = typeof globalThis.db === "undefined" ? undefined : globalThis.db;
    try {
        globalThis.db = coll.getDB();

        assert.soon(
            function () {
                assert.soon(
                    function () {
                        return assert.commandWorked(sh._adminCommand({balancerCollectionStatus: ns}, true))
                            .balancerCompliant;
                    },
                    "Timed out waiting for the collection to be balanced",
                    timeout,
                    interval,
                );

                // (SERVER-67301) Wait for orphans counter to be 0 to account for potential stale
                // orphans count
                sh.disableBalancing(coll);
                assert.soon(
                    function () {
                        return coll.aggregate(orphanDocsPipeline).toArray()[0].totalNumOrphanDocs === 0;
                    },
                    "Timed out waiting for orphans counter to be 0",
                    timeout,
                    interval,
                );
                sh.enableBalancing(coll);

                // (SERVER-70602) Wait for some balancing rounds to avoid balancerCollectionStatus
                // reporting balancerCompliant too early
                for (let i = 0; i < 3; ++i) {
                    sh.awaitBalancerRound(timeout, interval);
                }

                return assert.commandWorked(sh._adminCommand({balancerCollectionStatus: ns}, true)).balancerCompliant;
            },
            "Timed out waiting for collection to be balanced and orphans counter to be 0",
            timeout,
            interval,
        );
    } finally {
        globalThis.db = oldDb;
    }
};

/**
 * Verifies if given collection is properly balanced according to the data size aware balancing
 * policy
 */
sh.verifyCollectionIsBalanced = function (coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }

    let oldDb = globalThis.db;
    try {
        globalThis.db = coll.getDB();

        const configDB = sh._getConfigDB();
        const ns = coll.getFullName();
        const collection = configDB.collections.findOne({_id: ns});

        let collSizeOnShards = [];
        let shards = [];
        const collStatsPipeline = [
            {"$collStats": {"storageStats": {}}},
            {
                "$project": {
                    "shard": true,
                    "storageStats": {"count": true, "size": true, "avgObjSize": true, "numOrphanDocs": true},
                },
            },
            {"$sort": {"shard": 1}},
        ];

        let kChunkSize = 1024 * 1024 * assert.commandWorked(sh._adminCommand({balancerCollectionStatus: ns})).chunkSize;

        // Get coll size per shard
        const storageStats = coll.aggregate(collStatsPipeline).toArray();
        coll.aggregate(collStatsPipeline).forEach((shardStats) => {
            shards.push(shardStats["shard"]);
            const collSize =
                (shardStats["storageStats"]["count"] - shardStats["storageStats"]["numOrphanDocs"]) *
                shardStats["storageStats"]["avgObjSize"];
            collSizeOnShards.push(collSize);
        });

        let errorMsg =
            "Collection not balanced. collection= " +
            tojson(collection) +
            ", shards= " +
            tojson(shards) +
            ", collSizeOnShards=" +
            tojson(collSizeOnShards) +
            ", storageStats=" +
            tojson(storageStats) +
            ", kChunkSize=" +
            tojson(kChunkSize);

        assert.lte(Math.max(...collSizeOnShards) - Math.min(...collSizeOnShards), 3 * kChunkSize, errorMsg);
    } finally {
        globalThis.db = oldDb;
    }
};

/*
 * Can call _lastMigration( coll ), _lastMigration( db ), _lastMigration( st ), _lastMigration(
 * mongos )
 */
sh._lastMigration = function (ns) {
    let coll = null;
    let dbase = null;
    let config = null;

    if (!ns) {
        config = globalThis.db.getSiblingDB("config");
    } else if (ns instanceof DBCollection) {
        coll = ns;
        config = coll.getDB().getSiblingDB("config");
    } else if (ns instanceof DB) {
        dbase = ns;
        config = dbase.getSiblingDB("config");
    } else if (ns instanceof ShardingTest) {
        config = ns.s.getDB("config");
    } else if (ns instanceof Mongo) {
        config = ns.getDB("config");
    } else {
        // String namespace
        ns = ns + "";
        if (ns.indexOf(".") > 0) {
            config = globalThis.db.getSiblingDB("config");
            coll = globalThis.db.getMongo().getCollection(ns);
        } else {
            config = globalThis.db.getSiblingDB("config");
            dbase = globalThis.db.getSiblingDB(ns);
        }
    }

    let searchDoc = {what: /^moveChunk/};
    if (coll) searchDoc.ns = coll + "";
    if (dbase) searchDoc.ns = new RegExp("^" + dbase + "\\.");

    let cursor = config.changelog.find(searchDoc).sort({time: -1}).limit(1);
    if (cursor.hasNext()) return cursor.next();
    else return null;
};

sh.addShardTag = function (shard, tag) {
    let result = sh.addShardToZone(shard, tag);
    if (result.code != ErrorCodes.CommandNotFound) {
        return result;
    }

    let config = sh._getConfigDB();
    if (config.shards.findOne({_id: shard}) == null) {
        throw Error("can't find a shard with name: " + shard);
    }
    return assert.commandWorked(
        config.shards.update({_id: shard}, {$addToSet: {tags: tag}}, {writeConcern: {w: "majority", wtimeout: 60000}}),
    );
};

sh.removeShardTag = function (shard, tag) {
    let result = sh.removeShardFromZone(shard, tag);
    if (result.code != ErrorCodes.CommandNotFound) {
        return result;
    }

    let config = sh._getConfigDB();
    if (config.shards.findOne({_id: shard}) == null) {
        throw Error("can't find a shard with name: " + shard);
    }
    return assert.commandWorked(
        config.shards.update({_id: shard}, {$pull: {tags: tag}}, {writeConcern: {w: "majority", wtimeout: 60000}}),
    );
};

sh.addTagRange = function (ns, min, max, tag) {
    let result = sh.updateZoneKeyRange(ns, min, max, tag);
    if (result.code != ErrorCodes.CommandNotFound) {
        return result;
    }

    if (bsonWoCompare(min, max) == 0) {
        throw new Error("min and max cannot be the same");
    }

    let config = sh._getConfigDB();
    return assert.commandWorked(
        config.tags.update(
            {_id: {ns, min}},
            {_id: {ns, min}, ns, min, max, tag},
            {upsert: true, writeConcern: {w: "majority", wtimeout: 60000}},
        ),
    );
};

sh.removeTagRange = function (ns, min, max) {
    return sh._getConfigDB().adminCommand({updateZoneKeyRange: ns, min, max, zone: null});
};

sh.addShardToZone = function (shardName, zoneName) {
    return sh._getConfigDB().adminCommand({addShardToZone: shardName, zone: zoneName});
};

sh.removeShardFromZone = function (shardName, zoneName) {
    return sh._getConfigDB().adminCommand({removeShardFromZone: shardName, zone: zoneName});
};

sh.updateZoneKeyRange = function (ns, min, max, zoneName) {
    return sh._getConfigDB().adminCommand({updateZoneKeyRange: ns, min, max, zone: zoneName});
};

sh.removeRangeFromZone = function (ns, min, max) {
    return sh._getConfigDB().adminCommand({updateZoneKeyRange: ns, min, max, zone: null});
};

sh.getBalancerWindow = function (configDB) {
    if (configDB === undefined) configDB = globalThis.db.getSiblingDB("config");
    let settings = configDB.settings.findOne({_id: "balancer"});
    if (settings == null) {
        return null;
    }
    if (settings.hasOwnProperty("activeWindow")) {
        return settings.activeWindow;
    }
    return null;
};

sh.getActiveMigrations = function (configDB) {
    if (configDB === undefined) configDB = globalThis.db.getSiblingDB("config");
    let activeLocks = configDB.locks.find({state: {$eq: 2}});
    let result = [];
    if (activeLocks != null) {
        activeLocks.forEach(function (lock) {
            result.push({_id: lock._id, when: lock.when});
        });
    }
    return result;
};

sh.getRecentFailedRounds = function (configDB) {
    if (configDB === undefined) configDB = globalThis.db.getSiblingDB("config");
    let balErrs = configDB.actionlog.find({what: "balancer.round"}).sort({time: -1}).limit(5);
    let result = {count: 0, lastErr: "", lastTime: " "};
    if (balErrs != null) {
        balErrs.forEach(function (r) {
            if (r.details.errorOccurred) {
                result.count += 1;
                if (result.count == 1) {
                    result.lastErr = r.details.errmsg;
                    result.lastTime = r.time;
                }
            }
        });
    }
    return result;
};

/**
 * Returns a summary of chunk migrations that was completed either successfully or not
 * since yesterday. The format is an array of 2 arrays, where the first array contains
 * the successful cases, and the second array contains the failure cases.
 */
sh.getRecentMigrations = function (configDB) {
    if (configDB === undefined) configDB = sh._getConfigDB();
    let yesterday = new Date(new Date() - 24 * 60 * 60 * 1000);

    // Successful migrations.
    let result = configDB.changelog
        .aggregate([
            {
                $match: {
                    time: {$gt: yesterday},
                    what: "moveChunk.from",
                    "details.errmsg": {$exists: false},
                    "details.note": "success",
                },
            },
            {$group: {_id: {msg: "$details.errmsg"}, count: {$sum: 1}}},
            {$project: {_id: {$ifNull: ["$_id.msg", "Success"]}, count: "$count"}},
        ])
        .toArray();

    // Failed migrations.
    result = result.concat(
        configDB.changelog
            .aggregate([
                {
                    $match: {
                        time: {$gt: yesterday},
                        what: "moveChunk.from",
                        $or: [{"details.errmsg": {$exists: true}}, {"details.note": {$ne: "success"}}],
                    },
                },
                {
                    $group: {
                        _id: {msg: "$details.errmsg", from: "$details.from", to: "$details.to"},
                        count: {$sum: 1},
                    },
                },
                {
                    $project: {
                        _id: {$ifNull: ["$_id.msg", "aborted"]},
                        from: "$_id.from",
                        to: "$_id.to",
                        count: "$count",
                    },
                },
            ])
            .toArray(),
    );

    return result;
};

sh._shardingStatusStr = function (indent, s) {
    // convert from logical indentation to actual num of chars
    if (indent == 0) {
        indent = 0;
    } else if (indent == 1) {
        indent = 2;
    } else {
        indent = (indent - 1) * 8;
    }
    return indentStr(indent, s) + "\n";
};

sh.balancerCollectionStatus = function (coll) {
    return sh._adminCommand({balancerCollectionStatus: coll}, true);
};

sh.configureCollectionBalancing = function (coll, opts) {
    let cmd = {configureCollectionBalancing: coll};
    if (opts) {
        cmd = Object.assign(cmd, opts);
    }
    return sh._adminCommand(cmd, true);
};

function printShardingStatus(configDB, verbose) {
    // configDB is a DB object that contains the sharding metadata of interest.
    // Defaults to the db named "config" on the current connection.
    if (configDB === undefined) {
        configDB = globalThis.db.getSiblingDB("config");
    }

    let version = configDB.getCollection("version").findOne();
    if (version == null) {
        print(
            "printShardingStatus: this db does not have sharding enabled. be sure you are connecting to a mongos from the shell and not to a mongod.",
        );
        return;
    }

    let raw = "";
    let output = function (indent, s) {
        raw += sh._shardingStatusStr(indent, s);
    };
    output(0, "--- Sharding Status --- ");
    output(1, "sharding version: " + tojson(configDB.getCollection("version").findOne()));

    output(1, "shards:");
    configDB.shards
        .find()
        .sort({_id: 1})
        .forEach(function (z) {
            output(2, tojsononeline(z));
        });

    // (most recently) active mongoses
    let mongosActiveThresholdMs = 60000;
    let mostRecentMongos = configDB.mongos.find().sort({ping: -1}).limit(1);
    let mostRecentMongosTime = null;
    let mongosAdjective = "most recently active";
    if (mostRecentMongos.hasNext()) {
        mostRecentMongosTime = mostRecentMongos.next().ping;
        // Mongoses older than the threshold are the most recent, but cannot be
        // considered "active" mongoses. (This is more likely to be an old(er)
        // configdb dump, or all the mongoses have been stopped.)
        if (mostRecentMongosTime.getTime() >= Date.now() - mongosActiveThresholdMs) {
            mongosAdjective = "active";
        }
    }

    output(1, mongosAdjective + " mongoses:");
    if (mostRecentMongosTime === null) {
        output(2, "none");
    } else {
        let recentMongosQuery = {
            ping: {
                $gt: (function () {
                    let d = mostRecentMongosTime;
                    d.setTime(d.getTime() - mongosActiveThresholdMs);
                    return d;
                })(),
            },
        };

        if (verbose) {
            configDB.mongos
                .find(recentMongosQuery)
                .sort({ping: -1})
                .forEach(function (z) {
                    output(2, tojsononeline(z));
                });
        } else {
            configDB.mongos
                .aggregate([
                    {$match: recentMongosQuery},
                    {$group: {_id: "$mongoVersion", num: {$sum: 1}}},
                    {$sort: {num: -1}},
                ])
                .forEach(function (z) {
                    output(2, tojson(z._id) + " : " + z.num);
                });
        }
    }

    output(1, "automerge:");

    output(2, "Currently enabled: " + (sh.shouldAutoMerge(configDB) ? "yes" : "no"));

    output(1, "balancer:");

    let balancerEnabledString;
    let balancerRunning;

    const balancerStatus = configDB.adminCommand({balancerStatus: 1});
    if (!balancerStatus.ok) {
        // If the call to balancerStatus returns CommandNotFound, we indicate that the balancer
        // being enabled is currently unknown, since CommandNotFound implies we're running this
        // command on a standalone mongod. All other error statuses return "no" for historical
        // reasons.
        balancerEnabledString = balancerStatus.code == ErrorCodes.CommandNotFound ? "unknown" : "no";
        balancerRunning = false;
    } else {
        balancerEnabledString = sh.getBalancerState(configDB, balancerStatus) ? "yes" : "no";
        balancerRunning = balancerStatus.inBalancerRound;
    }

    // Is the balancer currently enabled
    output(2, "Currently enabled: " + balancerEnabledString);

    // Is the balancer currently active
    output(2, "Currently running: " + (balancerRunning ? "yes" : "no"));

    // Output the balancer window
    let balSettings = sh.getBalancerWindow(configDB);
    if (balSettings) {
        output(
            3,
            "Balancer active window is set between " +
                balSettings.start +
                " and " +
                balSettings.stop +
                " server local time",
        );
    }

    // Output the list of active migrations
    let activeMigrations = sh.getActiveMigrations(configDB);
    if (activeMigrations.length > 0) {
        output(2, "Collections with active migrations: ");
        activeMigrations.forEach(function (migration) {
            output(3, migration._id + " started at " + migration.when);
        });
    }

    // Actionlog and version checking only works on 2.7 and greater
    let versionHasActionlog = false;
    let metaDataVersion = configDB.getCollection("version").findOne().currentVersion;
    if (metaDataVersion > 5) {
        versionHasActionlog = true;
    }
    if (metaDataVersion == 5) {
        let verArray = globalThis.db.getServerBuildInfo().rawData().versionArray;
        if (verArray[0] == 2 && verArray[1] > 6) {
            versionHasActionlog = true;
        }
    }

    if (versionHasActionlog) {
        // Review config.actionlog for errors
        let actionReport = sh.getRecentFailedRounds(configDB);
        // Always print the number of failed rounds
        output(2, "Failed balancer rounds in last 5 attempts: " + actionReport.count);

        // Only print the errors if there are any
        if (actionReport.count > 0) {
            output(2, "Last reported error: " + actionReport.lastErr);
            output(2, "Time of reported error: " + actionReport.lastTime);
        }

        output(2, "Migration results for the last 24 hours: ");
        let migrations = sh.getRecentMigrations(configDB);
        if (migrations.length > 0) {
            migrations.forEach(function (x) {
                if (x._id === "Success") {
                    output(3, x.count + " : " + x._id);
                } else {
                    output(3, x.count + " : Failed with error '" + x._id + "', from " + x.from + " to " + x.to);
                }
            });
        } else {
            output(3, "No recent migrations");
        }
    }

    output(1, "databases:");

    let databases = configDB.databases.find().sort({name: 1}).toArray();

    // Special case the config db, since it doesn't have a record in config.databases.
    databases.push({"_id": "config", "primary": "config", "partitioned": true});
    databases.sort(function (a, b) {
        return a["_id"] > b["_id"];
    });

    databases.forEach(function (db) {
        let truthy = function (value) {
            return !!value;
        };
        let nonBooleanNote = function (name, value) {
            // If the given value is not a boolean, return a string of the
            // form " (<name>: <value>)", where <value> is converted to JSON.
            let t = typeof value;
            let s = "";
            if (t != "boolean" && t != "undefined") {
                s = " (" + name + ": " + tojson(value) + ")";
            }
            return s;
        };

        output(2, tojsononeline(db, "", true));

        configDB.collections
            .find({_id: new RegExp("^" + RegExp.escape(db._id) + "\\.")})
            .sort({_id: 1})
            .forEach(function (coll) {
                // Checking for '!dropped' to ensure mongo shell compatibility with earlier
                // versions of the server
                if (!coll.dropped) {
                    output(3, coll._id);
                    output(4, "shard key: " + tojson(coll.key));
                    output(4, "unique: " + truthy(coll.unique) + nonBooleanNote("unique", coll.unique));
                    output(4, "balancing: " + !truthy(coll.noBalance) + nonBooleanNote("noBalance", coll.noBalance));
                    output(4, "chunks:");

                    const chunksMatchPredicate = coll.hasOwnProperty("timestamp") ? {uuid: coll.uuid} : {ns: coll._id};

                    const res = configDB.chunks
                        .aggregate(
                            {$match: chunksMatchPredicate},
                            {$group: {_id: "$shard", cnt: {$sum: 1}}},
                            {$project: {_id: 0, shard: "$_id", nChunks: "$cnt"}},
                            {$sort: {shard: 1}},
                        )
                        .toArray();

                    let totalChunks = 0;
                    res.forEach(function (z) {
                        totalChunks += z.nChunks;
                        output(5, z.shard + "\t" + z.nChunks);
                    });

                    if (totalChunks < 20 || verbose) {
                        configDB.chunks
                            .find(chunksMatchPredicate)
                            .sort({min: 1})
                            .forEach(function (chunk) {
                                output(
                                    4,
                                    tojson(chunk.min) +
                                        " -->> " +
                                        tojson(chunk.max) +
                                        " on : " +
                                        chunk.shard +
                                        " " +
                                        tojson(chunk.lastmod) +
                                        " " +
                                        (chunk.jumbo ? "jumbo " : ""),
                                );
                            });
                    } else {
                        output(4, "too many chunks to print, use verbose if you want to force print");
                    }

                    configDB.tags
                        .find({ns: coll._id})
                        .sort({min: 1})
                        .forEach(function (tag) {
                            output(4, " tag: " + tag.tag + "  " + tojson(tag.min) + " -->> " + tojson(tag.max));
                        });
                }
            });
    });

    print(raw);
}

export {sh, printShardingStatus};
