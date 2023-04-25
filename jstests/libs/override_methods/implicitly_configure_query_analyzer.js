/**
 * Loading this file overrides DB.prototype.{createCollection, getCollection},
 * DBCollection.prototype.{drop, createIndex} to enable query analyzer.
 * Some collections will be sharded so that query sampling can be tested both with and without
 * sharding.
 */

(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/override_methods/shard_collection_util.js");

const kShardProbability = 0.5;
const kSampleRate = 1000;  // per second.

// Save a reference to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
var originalCreateCollection = DB.prototype.createCollection;
var originalGetCollection = DB.prototype.getCollection;
var originalDBCollectionDrop = DBCollection.prototype.drop;

function isUnsupportedNamespace(dbName, collName) {
    const ns = dbName + "." + collName;
    for (let denylistedNs of denylistedNamespaces) {
        if (ns.match(denylistedNs)) {
            return true;
        }
    }
    return false;
}

function configureQueryAnalyzer({db, collName}) {
    const dbName = db.getName();
    const ns = dbName + "." + collName;
    assert(!isUnsupportedNamespace(dbName, collName), {dbName, collName});

    let result;
    try {
        result =
            db.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: kSampleRate});
    } catch (e) {
        print(`Failed to configure query analyzer: ${tojsononeline({ns, e})}`);
        if (!isNetworkError(e)) {
            throw e;
        }
    }
    if (!result.ok) {
        if (result.code === ErrorCodes.CommandNotFound ||
            result.code === ErrorCodes.NamespaceNotFound ||
            result.code === ErrorCodes.CommandNotSupportedOnView) {
            print(`Failed to configure query analyzer: ${tojsononeline({ns, result})}`);
            return;
        }
        assert.commandWorked(result);
    }
}

DB.prototype.createCollection = function() {
    const result = originalCreateCollection.apply(this, arguments);

    if (!result.ok || arguments.length < 2 || arguments[1] == null || !isObject(arguments[1])) {
        return result;
    }

    const dbName = this.getName();
    const collName = arguments[0];

    if (isUnsupportedNamespace(dbName, collName)) {
        return result;
    }

    // Attempt to enable sharding on database and collection if not already done;
    // only shard some of the time, so that query sampling is also tested on unsharded collections.
    if (Math.random() < kShardProbability) {
        if (arguments[1].timeseries && arguments[1].timeseries.timeField) {
            const timeField = arguments[1]["timeseries"]["timeField"];
            ShardingOverrideCommon.shardCollectionWithSpec({
                db: this,
                collName,
                shardKey: {[timeField]: 1},
                timeseriesSpec: arguments[1]["timeseries"]
            });
        } else {
            ShardingOverrideCommon.shardCollection(originalGetCollection.apply(this, [collName]));
        }
    }
    configureQueryAnalyzer({db: this, collName});

    return result;
};

DB.prototype.getCollection = function() {
    const collection = originalGetCollection.apply(this, arguments);

    const dbName = this.getName();
    const collName = collection.getName();

    if (isUnsupportedNamespace(dbName, collName)) {
        return collection;
    }

    const collStats = this.runCommand({collStats: collName});
    // If the collection is already sharded or is non-empty, do not try to create or shard it.
    if (collStats.sharded || collStats.count > 0) {
        return collection;
    }

    if (TestData.implicitlyShardOnCreateCollectionOnly) {
        // Don't shard or create the collection.
        return collection;
    }

    const result = originalCreateCollection.apply(this, [collName]);

    if (!result.ok)
        return collection;

    // Only shard some of the time, so that query sampling is also tested on unsharded collections.
    if (Math.random() < kShardProbability) {
        ShardingOverrideCommon.shardCollection(collection);
    }
    configureQueryAnalyzer({db: collection.getDB(), collName: collName});

    return collection;
};

DBCollection.prototype.drop = function() {
    const result = originalDBCollectionDrop.apply(this, arguments);

    if (!result || TestData.implicitlyShardOnCreateCollectionOnly) {
        // Don't shard or create the collection.
        return result;
    }

    const db = this.getDB();
    const dbName = db.getName();
    const collName = this.getName();

    if (isUnsupportedNamespace(dbName, collName)) {
        return result;
    }

    assert.commandWorked(db.createCollection(collName));

    // Only shard some of the time, so that query sampling is also tested on unsharded collections.
    if (Math.random() < kShardProbability) {
        ShardingOverrideCommon.shardCollection(this);
    }
    configureQueryAnalyzer({db: db, collName: collName});

    return result;
};
})();
