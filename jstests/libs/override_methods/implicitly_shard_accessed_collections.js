/**
 * Loading this file overrides DB.prototype.getCollection() with a function that attempts to shard
 * the collection before returning it.
 *
 * The DB.prototype.getCollection() function is called whenever an undefined property is accessed
 * on the db object.
 *
 * DBCollection.prototype.drop() function will re-shard any non-denylisted collection that is
 * dropped in a sharded cluster.
 */

(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/fixture_helpers.js");                    // For 'FixtureHelpers'.
load("jstests/libs/override_methods/shard_collection_util.js");

// Save a reference to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
var originalGetCollection = DB.prototype.getCollection;
var originalCreateCollection = DB.prototype.createCollection;
var originalDBCollectionDrop = DBCollection.prototype.drop;
var originalStartParallelShell = startParallelShell;
var originalRunCommand = Mongo.prototype.runCommand;

DB.prototype.createCollection = function() {
    const createCollResult = originalCreateCollection.apply(this, arguments);

    if (!createCollResult.ok || arguments.length < 2 || arguments[1] == null ||
        !isObject(arguments[1]) || !arguments[1].timeseries || !arguments[1].timeseries.timeField) {
        return createCollResult;
    }

    // We check feature flags on both primary and secondaries in case a step down happens after this
    // check.
    const featureResults = FixtureHelpers
                               .runCommandOnAllShards({
                                   db: this.getSiblingDB('admin'),
                                   cmdObj: {getParameter: 1, featureFlagShardedTimeSeries: 1}
                               })
                               .map(result => assert.commandWorked(result));

    // The feature can only be used if the version associated with the feature is greater than or
    // equal to the FCV version. The getParameter does not consider the FCV value when checking for
    // whether the feature flag is enabled. So we run an additional getParameter command to fetch
    // the FCV state.
    const fcvResult =
        assert.commandWorked(FixtureHelpers.getPrimaryForNodeHostingDatabase(this).adminCommand(
            {getParameter: 1, featureCompatibilityVersion: 1}));
    const isTimeseriesShardingEnabled = featureResults.every(
        result => result.featureFlagShardedTimeSeries.value &&
            MongoRunner.compareBinVersions(fcvResult.featureCompatibilityVersion.version,
                                           result.featureFlagShardedTimeSeries.version) >= 0);
    if (!isTimeseriesShardingEnabled) {
        return createCollResult;
    }

    const timeField = arguments[1]["timeseries"]["timeField"];
    ShardingOverrideCommon.shardCollectionWithSpec({
        db: this,
        collName: arguments[0],
        shardKey: {[timeField]: 1},
        timeseriesSpec: arguments[1]["timeseries"]
    });

    return createCollResult;
};

DB.prototype.getCollection = function() {
    var collection = originalGetCollection.apply(this, arguments);

    // The following "collStats" command can behave unexpectedly when running in a causal
    // consistency suite with secondary read preference. "collStats" does not support causal
    // consistency, making it possible to see a stale view of the collection if run on a
    // secondary, potentially causing shardCollection() to be called when it shouldn't.
    // E.g. if the collection has just been sharded but not yet visible on the
    // secondary, we could end up calling shardCollection on it again, which would fail.
    //
    // The workaround is to use a TestData flag to temporarily bypass the read preference
    // override.
    const testDataDoNotOverrideReadPreferenceOriginal = TestData.doNotOverrideReadPreference;
    let collStats;

    try {
        TestData.doNotOverrideReadPreference = true;
        collStats = this.runCommand({collStats: collection.getName()});
    } finally {
        TestData.doNotOverrideReadPreference = testDataDoNotOverrideReadPreferenceOriginal;
    }

    // If the collection is already sharded or is non-empty, do not attempt to shard.
    if (collStats.sharded || collStats.count > 0) {
        return collection;
    }

    // Attempt to enable sharding on database and collection if not already done.
    if (!TestData.implicitlyShardOnCreateCollectionOnly) {
        ShardingOverrideCommon.shardCollection(collection);
    }

    return collection;
};

DBCollection.prototype.drop = function() {
    var dropResult = originalDBCollectionDrop.apply(this, arguments);

    // Attempt to enable sharding on database and collection if not already done.
    if (!TestData.implicitlyShardOnCreateCollectionOnly) {
        ShardingOverrideCommon.shardCollection(this);
    }

    return dropResult;
};

// The mapReduce command has a special requirement where the command must indicate the output
// collection is sharded, so we must be sure to add this information in this passthrough.
Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    // Skip any commands that are not mapReduce or do not have an 'out' option.
    if (typeof cmdObj !== 'object' || cmdObj === null ||
        (!cmdObj.hasOwnProperty('mapreduce') && !cmdObj.hasOwnProperty('mapReduce')) ||
        !cmdObj.hasOwnProperty('out')) {
        return originalRunCommand.apply(this, arguments);
    }

    const originalCmdObj = Object.merge({}, cmdObj);

    // SERVER-5448 'jsMode' is not supported through mongos. The 'jsMode' should not impact the
    // results at all, so can be safely deleted in the sharded environment.
    delete cmdObj.jsMode;

    // Modify the output options to specify that the collection is sharded.
    let outputSpec = cmdObj.out;
    if (typeof (outputSpec) === "string") {
        this.getDB(dbName)[outputSpec].drop();  // This will implicitly shard it.
        outputSpec = {replace: outputSpec};
    } else if (typeof (outputSpec) !== "object") {
        // This is a malformed command, just send it along.
        return originalRunCommand.apply(this, arguments);
    } else if (!outputSpec.hasOwnProperty("sharded")) {
        let outputColl = null;
        if (outputSpec.hasOwnProperty("replace")) {
            outputColl = outputSpec.replace;
        } else if (outputSpec.hasOwnProperty("merge")) {
            outputColl = outputSpec.merge;
        } else if (outputSpec.hasOwnProperty("reduce")) {
            outputColl = outputSpec.reduce;
        }

        if (outputColl === null) {
            // This is a malformed command, just send it along.
            return originalRunCommand.apply(this, arguments);
        }
        this.getDB(dbName)[outputColl].drop();  // This will implicitly shard it.
        outputSpec.sharded = true;
    }

    cmdObj.out = outputSpec;
    jsTestLog('Overriding mapReduce command. Original command: ' + tojson(originalCmdObj) +
              ' New command: ' + tojson(cmdObj));
    return originalRunCommand.apply(this, arguments);
};

// Tests may use a parallel shell to run the "drop" command concurrently with other
// operations. This can cause the "shardCollection" command to return a
// ConflictingOperationInProgress error response.
startParallelShell = function() {
    testMayRunDropInParallel = true;
    return originalStartParallelShell.apply(this, arguments);
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_shard_accessed_collections.js");
}());
