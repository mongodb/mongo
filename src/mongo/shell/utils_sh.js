sh = function() {
    return "try sh.help();";
};

sh._checkMongos = function() {
    var x = db.runCommand("ismaster");
    if (x.msg != "isdbgrid")
        throw Error("not connected to a mongos");
};

sh._checkFullName = function(fullName) {
    assert(fullName, "need a full name");
    assert(fullName.indexOf(".") > 0, "name needs to be fully qualified <db>.<collection>'");
};

sh._adminCommand = function(cmd, skipCheck) {
    if (!skipCheck)
        sh._checkMongos();
    return db.getSisterDB("admin").runCommand(cmd);
};

sh._getConfigDB = function() {
    sh._checkMongos();
    return db.getSiblingDB("config");
};

sh._dataFormat = function(bytes) {
    if (bytes < 1024)
        return Math.floor(bytes) + "B";
    if (bytes < 1024 * 1024)
        return Math.floor(bytes / 1024) + "KiB";
    if (bytes < 1024 * 1024 * 1024)
        return Math.floor((Math.floor(bytes / 1024) / 1024) * 100) / 100 + "MiB";
    return Math.floor((Math.floor(bytes / (1024 * 1024)) / 1024) * 100) / 100 + "GiB";
};

sh._collRE = function(coll) {
    return RegExp("^" + RegExp.escape(coll + "") + "-.*");
};

sh._pchunk = function(chunk) {
    return "[" + tojson(chunk.min) + " -> " + tojson(chunk.max) + "]";
};

/**
 * Internal method to write the balancer state to the config.settings collection. Should not be used
 * directly, instead go through the start/stopBalancer calls and the balancerStart/Stop commands.
 */
sh._writeBalancerStateDeprecated = function(onOrNot) {
    return assert.writeOK(
        sh._getConfigDB().settings.update({_id: 'balancer'},
                                          {$set: {stopped: onOrNot ? false : true}},
                                          {upsert: true, writeConcern: {w: 'majority'}}));
};

sh.help = function() {
    print("\tsh.addShard( host )                       server:port OR setname/server:port");
    print("\tsh.addShardToZone(shard,zone)             adds the shard to the zone");
    print("\tsh.updateZoneKeyRange(fullName,min,max,zone)      " +
          "assigns the specified range of the given collection to a zone");
    print("\tsh.disableBalancing(coll)                 disable balancing on one collection");
    print("\tsh.enableBalancing(coll)                  re-enable balancing on one collection");
    print("\tsh.enableSharding(dbname)                 enables sharding on the database dbname");
    print("\tsh.getBalancerState()                     returns whether the balancer is enabled");
    print(
        "\tsh.isBalancerRunning()                    return true if the balancer has work in progress on any mongos");
    print(
        "\tsh.moveChunk(fullName,find,to)            move the chunk where 'find' is to 'to' (name of shard)");
    print("\tsh.removeShardFromZone(shard,zone)      removes the shard from zone");
    print(
        "\tsh.removeRangeFromZone(fullName,min,max)   removes the range of the given collection from any zone");
    print("\tsh.shardCollection(fullName,key,unique,options)   shards the collection");
    print(
        "\tsh.splitAt(fullName,middle)               splits the chunk that middle is in at middle");
    print(
        "\tsh.splitFind(fullName,find)               splits the chunk that find is in at the median");
    print(
        "\tsh.startBalancer()                        starts the balancer so chunks are balanced automatically");
    print("\tsh.status()                               prints a general overview of the cluster");
    print(
        "\tsh.stopBalancer()                         stops the balancer so chunks are not balanced automatically");
    print("\tsh.disableAutoSplit()                   disable autoSplit on one collection");
    print("\tsh.enableAutoSplit()                    re-enable autoSplit on one collection");
    print("\tsh.getShouldAutoSplit()                 returns whether autosplit is enabled");
};

sh.status = function(verbose, configDB) {
    // TODO: move the actual command here
    printShardingStatus(configDB, verbose);
};

sh.addShard = function(url) {
    return sh._adminCommand({addShard: url}, true);
};

sh.enableSharding = function(dbname) {
    assert(dbname, "need a valid dbname");
    return sh._adminCommand({enableSharding: dbname});
};

sh.shardCollection = function(fullName, key, unique, options) {
    sh._checkFullName(fullName);
    assert(key, "need a key");
    assert(typeof(key) == "object", "key needs to be an object");

    var cmd = {shardCollection: fullName, key: key};
    if (unique)
        cmd.unique = true;
    if (options) {
        if (typeof(options) !== "object") {
            throw new Error("options must be an object");
        }
        Object.extend(cmd, options);
    }

    return sh._adminCommand(cmd);
};

sh.splitFind = function(fullName, find) {
    sh._checkFullName(fullName);
    return sh._adminCommand({split: fullName, find: find});
};

sh.splitAt = function(fullName, middle) {
    sh._checkFullName(fullName);
    return sh._adminCommand({split: fullName, middle: middle});
};

sh.moveChunk = function(fullName, find, to) {
    sh._checkFullName(fullName);
    return sh._adminCommand({moveChunk: fullName, find: find, to: to});
};

sh.setBalancerState = function(isOn) {
    if (isOn) {
        return sh.startBalancer();
    } else {
        return sh.stopBalancer();
    }
};

sh.getBalancerState = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    var x = configDB.settings.findOne({_id: "balancer"});
    if (x == null)
        return true;
    return !x.stopped;
};

sh.isBalancerRunning = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();

    var result = configDB.adminCommand({balancerStatus: 1});
    return assert.commandWorked(result).inBalancerRound;
};

sh.stopBalancer = function(timeoutMs, interval) {
    timeoutMs = timeoutMs || 60000;

    var result = db.adminCommand({balancerStop: 1, maxTimeMS: timeoutMs});
    return assert.commandWorked(result);
};

sh.startBalancer = function(timeoutMs, interval) {
    timeoutMs = timeoutMs || 60000;

    var result = db.adminCommand({balancerStart: 1, maxTimeMS: timeoutMs});
    return assert.commandWorked(result);
};

sh.enableAutoSplit = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    return assert.writeOK(
        configDB.settings.update({_id: 'autosplit'},
                                 {$set: {enabled: true}},
                                 {upsert: true, writeConcern: {w: 'majority', wtimeout: 30000}}));
};

sh.disableAutoSplit = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    return assert.writeOK(
        configDB.settings.update({_id: 'autosplit'},
                                 {$set: {enabled: false}},
                                 {upsert: true, writeConcern: {w: 'majority', wtimeout: 30000}}));
};

sh.getShouldAutoSplit = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    var autosplit = configDB.settings.findOne({_id: 'autosplit'});
    if (autosplit == null) {
        return true;
    }
    return autosplit.enabled;
};

sh.waitForPingChange = function(activePings, timeout, interval) {
    var isPingChanged = function(activePing) {
        var newPing = sh._getConfigDB().mongos.findOne({_id: activePing._id});
        return !newPing || newPing.ping + "" != activePing.ping + "";
    };

    // First wait for all active pings to change, so we're sure a settings reload
    // happened

    // Timeout all pings on the same clock
    var start = new Date();

    var remainingPings = [];
    for (var i = 0; i < activePings.length; i++) {
        var activePing = activePings[i];
        print("Waiting for active host " + activePing._id +
              " to recognize new settings... (ping : " + activePing.ping + ")");

        // Do a manual timeout here, avoid scary assert.soon errors
        var timeout = timeout || 30000;
        var interval = interval || 200;
        while (isPingChanged(activePing) != true) {
            if ((new Date()).getTime() - start.getTime() > timeout) {
                print("Waited for active ping to change for host " + activePing._id +
                      ", a migration may be in progress or the host may be down.");
                remainingPings.push(activePing);
                break;
            }
            sleep(interval);
        }
    }

    return remainingPings;
};

sh.disableBalancing = function(coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    var dbase = db;
    if (coll instanceof DBCollection) {
        dbase = coll.getDB();
    } else {
        sh._checkMongos();
    }

    return assert.writeOK(dbase.getSisterDB("config").collections.update(
        {_id: coll + ""},
        {$set: {"noBalance": true}},
        {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.enableBalancing = function(coll) {
    if (coll === undefined) {
        throw Error("Must specify collection");
    }
    var dbase = db;
    if (coll instanceof DBCollection) {
        dbase = coll.getDB();
    } else {
        sh._checkMongos();
    }

    return assert.writeOK(dbase.getSisterDB("config").collections.update(
        {_id: coll + ""},
        {$set: {"noBalance": false}},
        {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

/*
 * Can call _lastMigration( coll ), _lastMigration( db ), _lastMigration( st ), _lastMigration(
 * mongos )
 */
sh._lastMigration = function(ns) {

    var coll = null;
    var dbase = null;
    var config = null;

    if (!ns) {
        config = db.getSisterDB("config");
    } else if (ns instanceof DBCollection) {
        coll = ns;
        config = coll.getDB().getSisterDB("config");
    } else if (ns instanceof DB) {
        dbase = ns;
        config = dbase.getSisterDB("config");
    } else if (ns instanceof ShardingTest) {
        config = ns.s.getDB("config");
    } else if (ns instanceof Mongo) {
        config = ns.getDB("config");
    } else {
        // String namespace
        ns = ns + "";
        if (ns.indexOf(".") > 0) {
            config = db.getSisterDB("config");
            coll = db.getMongo().getCollection(ns);
        } else {
            config = db.getSisterDB("config");
            dbase = db.getSisterDB(ns);
        }
    }

    var searchDoc = {what: /^moveChunk/};
    if (coll)
        searchDoc.ns = coll + "";
    if (dbase)
        searchDoc.ns = new RegExp("^" + dbase + "\\.");

    var cursor = config.changelog.find(searchDoc).sort({time: -1}).limit(1);
    if (cursor.hasNext())
        return cursor.next();
    else
        return null;
};

sh.addShardTag = function(shard, tag) {
    var result = sh.addShardToZone(shard, tag);
    if (result.code != ErrorCodes.CommandNotFound) {
        return result;
    }

    var config = sh._getConfigDB();
    if (config.shards.findOne({_id: shard}) == null) {
        throw Error("can't find a shard with name: " + shard);
    }
    return assert.writeOK(config.shards.update(
        {_id: shard}, {$addToSet: {tags: tag}}, {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.removeShardTag = function(shard, tag) {
    var result = sh.removeShardFromZone(shard, tag);
    if (result.code != ErrorCodes.CommandNotFound) {
        return result;
    }

    var config = sh._getConfigDB();
    if (config.shards.findOne({_id: shard}) == null) {
        throw Error("can't find a shard with name: " + shard);
    }
    return assert.writeOK(config.shards.update(
        {_id: shard}, {$pull: {tags: tag}}, {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.addTagRange = function(ns, min, max, tag) {
    var result = sh.updateZoneKeyRange(ns, min, max, tag);
    if (result.code != ErrorCodes.CommandNotFound) {
        return result;
    }

    if (bsonWoCompare(min, max) == 0) {
        throw new Error("min and max cannot be the same");
    }

    var config = sh._getConfigDB();
    return assert.writeOK(
        config.tags.update({_id: {ns: ns, min: min}},
                           {_id: {ns: ns, min: min}, ns: ns, min: min, max: max, tag: tag},
                           {upsert: true, writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.removeTagRange = function(ns, min, max, tag) {
    var result = sh.removeRangeFromZone(ns, min, max);
    if (result.code != ErrorCodes.CommandNotFound) {
        return result;
    }

    var config = sh._getConfigDB();
    // warn if the namespace does not exist, even dropped
    if (config.collections.findOne({_id: ns}) == null) {
        print("Warning: can't find the namespace: " + ns + " - collection likely never sharded");
    }
    // warn if the tag being removed is still in use
    if (config.shards.findOne({tags: tag})) {
        print("Warning: tag still in use by at least one shard");
    }
    // max and tag criteria not really needed, but including them avoids potentially unexpected
    // behavior.
    return assert.writeOK(config.tags.remove({_id: {ns: ns, min: min}, max: max, tag: tag},
                                             {writeConcern: {w: 'majority', wtimeout: 60000}}));
};

sh.addShardToZone = function(shardName, zoneName) {
    return sh._getConfigDB().adminCommand({addShardToZone: shardName, zone: zoneName});
};

sh.removeShardFromZone = function(shardName, zoneName) {
    return sh._getConfigDB().adminCommand({removeShardFromZone: shardName, zone: zoneName});
};

sh.updateZoneKeyRange = function(ns, min, max, zoneName) {
    return sh._getConfigDB().adminCommand(
        {updateZoneKeyRange: ns, min: min, max: max, zone: zoneName});
};

sh.removeRangeFromZone = function(ns, min, max) {
    return sh._getConfigDB().adminCommand({updateZoneKeyRange: ns, min: min, max: max, zone: null});
};

sh.getBalancerWindow = function(configDB) {
    if (configDB === undefined)
        configDB = db.getSiblingDB('config');
    var settings = configDB.settings.findOne({_id: 'balancer'});
    if (settings == null) {
        return null;
    }
    if (settings.hasOwnProperty("activeWindow")) {
        return settings.activeWindow;
    }
    return null;
};

sh.getActiveMigrations = function(configDB) {
    if (configDB === undefined)
        configDB = db.getSiblingDB('config');
    var activeLocks = configDB.locks.find({state: {$eq: 2}});
    var result = [];
    if (activeLocks != null) {
        activeLocks.forEach(function(lock) {
            result.push({_id: lock._id, when: lock.when});
        });
    }
    return result;
};

sh.getRecentFailedRounds = function(configDB) {
    if (configDB === undefined)
        configDB = db.getSiblingDB('config');
    var balErrs = configDB.actionlog.find({what: "balancer.round"}).sort({time: -1}).limit(5);
    var result = {count: 0, lastErr: "", lastTime: " "};
    if (balErrs != null) {
        balErrs.forEach(function(r) {
            if (r.details.errorOccured) {
                result.count += 1;
                result.lastErr = r.details.errmsg;
                result.lastTime = r.time;
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
sh.getRecentMigrations = function(configDB) {
    if (configDB === undefined)
        configDB = sh._getConfigDB();
    var yesterday = new Date(new Date() - 24 * 60 * 60 * 1000);

    // Successful migrations.
    var result = configDB.changelog
                     .aggregate([
                         {
                           $match: {
                               time: {$gt: yesterday},
                               what: "moveChunk.from",
                               'details.errmsg': {$exists: false},
                               'details.note': 'success'
                           }
                         },
                         {$group: {_id: {msg: "$details.errmsg"}, count: {$sum: 1}}},
                         {$project: {_id: {$ifNull: ["$_id.msg", "Success"]}, count: "$count"}}
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
                      $or: [
                          {'details.errmsg': {$exists: true}},
                          {'details.note': {$ne: 'success'}}
                      ]
                  }
                },
                {
                  $group: {
                      _id: {msg: "$details.errmsg", from: "$details.from", to: "$details.to"},
                      count: {$sum: 1}
                  }
                },
                {
                  $project: {
                      _id: {$ifNull: ['$_id.msg', 'aborted']},
                      from: "$_id.from",
                      to: "$_id.to",
                      count: "$count"
                  }
                }
            ])
            .toArray());

    return result;
};

sh._shardingStatusStr = function(indent, s) {
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

function printShardingStatus(configDB, verbose) {
    // configDB is a DB object that contains the sharding metadata of interest.
    // Defaults to the db named "config" on the current connection.
    if (configDB === undefined)
        configDB = db.getSisterDB('config');

    var version = configDB.getCollection("version").findOne();
    if (version == null) {
        print(
            "printShardingStatus: this db does not have sharding enabled. be sure you are connecting to a mongos from the shell and not to a mongod.");
        return;
    }

    var raw = "";
    var output = function(indent, s) {
        raw += sh._shardingStatusStr(indent, s);
    };
    output(0, "--- Sharding Status --- ");
    output(1, "sharding version: " + tojson(configDB.getCollection("version").findOne()));

    output(1, "shards:");
    configDB.shards.find().sort({_id: 1}).forEach(function(z) {
        output(2, tojsononeline(z));
    });

    // (most recently) active mongoses
    var mongosActiveThresholdMs = 60000;
    var mostRecentMongos = configDB.mongos.find().sort({ping: -1}).limit(1);
    var mostRecentMongosTime = null;
    var mongosAdjective = "most recently active";
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
        var recentMongosQuery = {
            ping: {
                $gt: (function() {
                    var d = mostRecentMongosTime;
                    d.setTime(d.getTime() - mongosActiveThresholdMs);
                    return d;
                })()
            }
        };

        if (verbose) {
            configDB.mongos.find(recentMongosQuery).sort({ping: -1}).forEach(function(z) {
                output(2, tojsononeline(z));
            });
        } else {
            configDB.mongos
                .aggregate([
                    {$match: recentMongosQuery},
                    {$group: {_id: "$mongoVersion", num: {$sum: 1}}},
                    {$sort: {num: -1}}
                ])
                .forEach(function(z) {
                    output(2, tojson(z._id) + " : " + z.num);
                });
        }
    }

    output(1, "autosplit:");

    // Is autosplit currently enabled
    output(2, "Currently enabled: " + (sh.getShouldAutoSplit(configDB) ? "yes" : "no"));

    output(1, "balancer:");

    // Is the balancer currently enabled
    output(2, "Currently enabled:  " + (sh.getBalancerState(configDB) ? "yes" : "no"));

    // Is the balancer currently active
    var balancerRunning = "unknown";
    var balancerStatus = configDB.adminCommand({balancerStatus: 1});
    if (balancerStatus.code != ErrorCodes.CommandNotFound) {
        balancerRunning = balancerStatus.inBalancerRound ? "yes" : "no";
    }
    output(2, "Currently running:  " + balancerRunning);

    // Output the balancer window
    var balSettings = sh.getBalancerWindow(configDB);
    if (balSettings) {
        output(3,
               "Balancer active window is set between " + balSettings.start + " and " +
                   balSettings.stop + " server local time");
    }

    // Output the list of active migrations
    var activeMigrations = sh.getActiveMigrations(configDB);
    if (activeMigrations.length > 0) {
        output(2, "Collections with active migrations: ");
        activeMigrations.forEach(function(migration) {
            output(3, migration._id + " started at " + migration.when);
        });
    }

    // Actionlog and version checking only works on 2.7 and greater
    var versionHasActionlog = false;
    var metaDataVersion = configDB.getCollection("version").findOne().currentVersion;
    if (metaDataVersion > 5) {
        versionHasActionlog = true;
    }
    if (metaDataVersion == 5) {
        var verArray = db.serverBuildInfo().versionArray;
        if (verArray[0] == 2 && verArray[1] > 6) {
            versionHasActionlog = true;
        }
    }

    if (versionHasActionlog) {
        // Review config.actionlog for errors
        var actionReport = sh.getRecentFailedRounds(configDB);
        // Always print the number of failed rounds
        output(2, "Failed balancer rounds in last 5 attempts:  " + actionReport.count);

        // Only print the errors if there are any
        if (actionReport.count > 0) {
            output(2, "Last reported error:  " + actionReport.lastErr);
            output(2, "Time of Reported error:  " + actionReport.lastTime);
        }

        output(2, "Migration Results for the last 24 hours: ");
        var migrations = sh.getRecentMigrations(configDB);
        if (migrations.length > 0) {
            migrations.forEach(function(x) {
                if (x._id === "Success") {
                    output(3, x.count + " : " + x._id);
                } else {
                    output(3,
                           x.count + " : Failed with error '" + x._id + "', from " + x.from +
                               " to " + x.to);
                }
            });
        } else {
            output(3, "No recent migrations");
        }
    }

    output(1, "databases:");

    var databases = configDB.databases.find().sort({name: 1}).toArray();

    // Special case the config db, since it doesn't have a record in config.databases.
    databases.push({"_id": "config", "primary": "config", "partitioned": true});
    databases.sort(function(a, b) {
        return a["_id"] > b["_id"];
    });

    databases.forEach(function(db) {
        var truthy = function(value) {
            return !!value;
        };
        var nonBooleanNote = function(name, value) {
            // If the given value is not a boolean, return a string of the
            // form " (<name>: <value>)", where <value> is converted to JSON.
            var t = typeof(value);
            var s = "";
            if (t != "boolean" && t != "undefined") {
                s = " (" + name + ": " + tojson(value) + ")";
            }
            return s;
        };

        output(2, tojsononeline(db, "", true));

        if (db.partitioned) {
            configDB.collections.find({_id: new RegExp("^" + RegExp.escape(db._id) + "\\.")})
                .sort({_id: 1})
                .forEach(function(coll) {
                    if (!coll.dropped) {
                        output(3, coll._id);
                        output(4, "shard key: " + tojson(coll.key));
                        output(4,
                               "unique: " + truthy(coll.unique) +
                                   nonBooleanNote("unique", coll.unique));
                        output(4,
                               "balancing: " + !truthy(coll.noBalance) +
                                   nonBooleanNote("noBalance", coll.noBalance));
                        output(4, "chunks:");

                        res = configDB.chunks
                                  .aggregate({$match: {ns: coll._id}},
                                             {$group: {_id: "$shard", cnt: {$sum: 1}}},
                                             {$project: {_id: 0, shard: "$_id", nChunks: "$cnt"}},
                                             {$sort: {shard: 1}})
                                  .toArray();
                        var totalChunks = 0;
                        res.forEach(function(z) {
                            totalChunks += z.nChunks;
                            output(5, z.shard + "\t" + z.nChunks);
                        });

                        if (totalChunks < 20 || verbose) {
                            configDB.chunks.find({"ns": coll._id})
                                .sort({min: 1})
                                .forEach(function(chunk) {
                                    output(4,
                                           tojson(chunk.min) + " -->> " + tojson(chunk.max) +
                                               " on : " + chunk.shard + " " +
                                               tojson(chunk.lastmod) + " " +
                                               (chunk.jumbo ? "jumbo " : ""));
                                });
                        } else {
                            output(
                                4,
                                "too many chunks to print, use verbose if you want to force print");
                        }

                        configDB.tags.find({ns: coll._id}).sort({min: 1}).forEach(function(tag) {
                            output(4,
                                   " tag: " + tag.tag + "  " + tojson(tag.min) + " -->> " +
                                       tojson(tag.max));
                        });
                    }
                });
        }
    });

    print(raw);
}

function printShardingSizes(configDB) {
    // configDB is a DB object that contains the sharding metadata of interest.
    // Defaults to the db named "config" on the current connection.
    if (configDB === undefined)
        configDB = db.getSisterDB('config');

    var version = configDB.getCollection("version").findOne();
    if (version == null) {
        print("printShardingSizes : not a shard db!");
        return;
    }

    var raw = "";
    var output = function(indent, s) {
        raw += sh._shardingStatusStr(indent, s);
    };
    output(0, "--- Sharding Sizes --- ");
    output(1, "sharding version: " + tojson(configDB.getCollection("version").findOne()));

    output(1, "shards:");
    var shards = {};
    configDB.shards.find().forEach(function(z) {
        shards[z._id] = new Mongo(z.host);
        output(2, tojson(z));
    });

    var saveDB = db;
    output(1, "databases:");
    configDB.databases.find().sort({name: 1}).forEach(function(db) {
        output(2, tojson(db, "", true));

        if (db.partitioned) {
            configDB.collections.find({_id: new RegExp("^" + RegExp.escape(db._id) + "\.")})
                .sort({_id: 1})
                .forEach(function(coll) {
                    output(3, coll._id + " chunks:");
                    configDB.chunks.find({"ns": coll._id}).sort({min: 1}).forEach(function(chunk) {
                        var mydb = shards[chunk.shard].getDB(db._id);
                        var out = mydb.runCommand({
                            dataSize: coll._id,
                            keyPattern: coll.key,
                            min: chunk.min,
                            max: chunk.max
                        });
                        delete out.millis;
                        delete out.ok;

                        output(4,
                               tojson(chunk.min) + " -->> " + tojson(chunk.max) + " on : " +
                                   chunk.shard + " " + tojson(out));

                    });
                });
        }
    });

    print(raw);
}
