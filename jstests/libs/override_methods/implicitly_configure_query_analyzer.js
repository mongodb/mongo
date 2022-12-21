/**
 * Loading this file overrides DB.prototype.{createCollection, getCollection},
 * DBCollection.prototype.{drop, createIndex} to enable query analyzer.
 * Some collections will be sharded so that query sampling can be tested both with and without
 * sharding.
 */

(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/fixture_helpers.js");                    // For 'FixtureHelpers'.
load("jstests/libs/override_methods/shard_collection_util.js");

const kConfigSampledQueriesCollName = "sampledQueries";
const kConfigSampledQueriesDiffCollName = "sampledQueriesDiff";
const kShardProbability = 0.5;
const kPeriodicLogTimeInterval = 30000;  // milliseconds
const kSampleRate = 1000;                // per second
let firstLog = true;                     // set to false after the first log

var prevLogTime = Date.now();

// Save a reference to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
var originalCreateCollection = DB.prototype.createCollection;
var originalGetCollection = DB.prototype.getCollection;
var originalDBCollectionDrop = DBCollection.prototype.drop;
var originalDBCollectionCreateIndex = DBCollection.prototype.createIndex;

function configureQueryAnalyzer({db, collName}) {
    const ns = db.getName() + "." + collName;

    for (var denyNs of denylistedNamespaces) {
        if (ns.match(denyNs)) {
            return;
        }
    }

    let result;
    try {
        result =
            db.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: kSampleRate});

    } catch {
    }
    if (!result.ok) {
        if (result.code === ErrorCodes.CommandNotFound ||
            result.code === ErrorCodes.NamespaceNotFound) {
            print('configureQueryAnalyzer failed; continuing without configuring:');
            printjson(result);
        } else {
            print('configureQueryAnalyzer failed.');
            printjson(result);
        }
    }
}

function periodicLogStats(db) {
    const timeNow = Date.now();
    const cfgdb = db.getMongo().getDB('config');
    if (firstLog || timeNow - prevLogTime >= kPeriodicLogTimeInterval) {
        firstLog = false;
        const sampledQueriesStats = FixtureHelpers
                                        .runCommandOnAllShards({
                                            db: cfgdb,
                                            cmdObj: {collStats: kConfigSampledQueriesCollName},
                                        })
                                        .map((result) => assert.commandWorked(result));
        const sampledQueriesDiffStats =
            FixtureHelpers
                .runCommandOnAllShards({
                    db: cfgdb,
                    cmdObj: {collStats: kConfigSampledQueriesDiffCollName},
                })
                .map((result) => assert.commandWorked(result));
        print("analyze_shard_key sampledQueries counts:");
        sampledQueriesStats.forEach((result) => print(result.count));
        print("analyze_shard_key sampledQueriesDiff counts:");
        sampledQueriesDiffStats.forEach((result) => print(result.count));
        prevLogTime = timeNow;
    }
}

DB.prototype.createCollection = function() {
    periodicLogStats(this);
    const result = originalCreateCollection.apply(this, arguments);

    if (!result.ok || arguments.length < 2 || arguments[1] == null || !isObject(arguments[1])) {
        return result;
    }

    // Attempt to enable sharding on database and collection if not already done;
    // only shard some of the time, so that query sampling is also tested on unsharded collections.
    if (Math.random() < kShardProbability) {
        if (arguments[1].timeseries && arguments[1].timeseries.timeField) {
            const timeField = arguments[1]["timeseries"]["timeField"];
            ShardingOverrideCommon.shardCollectionWithSpec({
                db: this,
                collName: arguments[0],
                shardKey: {[timeField]: 1},
                timeseriesSpec: arguments[1]["timeseries"]
            });
        } else {
            ShardingOverrideCommon.shardCollection(
                originalGetCollection.apply(this, [arguments[0]]));
        }
    }
    configureQueryAnalyzer({db: this, collName: arguments[0]});

    return result;
};

DB.prototype.getCollection = function() {
    var collection = originalGetCollection.apply(this, arguments);

    if (TestData.implicitlyShardOnCreateCollectionOnly) {
        // Don't shard or create the collection.
        return collection;
    }

    var result;
    try {
        result = originalCreateCollection.apply(this, [collection.getName()]);
    } catch {
        // collection creation failed; already exists?
        return collection;
    }
    if (!result.ok)
        return collection;

    // Only shard some of the time, so that query sampling is also tested on unsharded collections.
    if (Math.random() < kShardProbability) {
        try {
            ShardingOverrideCommon.shardCollection(collection);
        } catch {
        }
    }
    configureQueryAnalyzer({db: collection.getDB(), collName: arguments[0]});

    return collection;
};

DBCollection.prototype.createIndex = function() {
    periodicLogStats(this.getDB());
    const result = originalDBCollectionCreateIndex.apply(this, arguments);

    if (!result.ok) {
        return result;
    }
    configureQueryAnalyzer({db: db, collName: this.getName()});
    return result;
};

DBCollection.prototype.drop = function() {
    periodicLogStats(this.getDB());
    const result = originalDBCollectionDrop.apply(this, arguments);

    if (!result || TestData.implicitlyShardOnCreateCollectionOnly) {
        // Don't shard or create the collection.
        return result;
    }

    var db = this.getDB();
    assert.commandWorked(db.createCollection(this.getName()));

    // Only shard some of the time, so that query sampling is also tested on unsharded collections.
    if (Math.random() < kShardProbability) {
        ShardingOverrideCommon.shardCollection(this);
    }
    configureQueryAnalyzer({db: db, collName: this.getName()});

    return result;
};
})();
