"use strict";

(function() {
'use strict';

load("jstests/libs/discover_topology.js");  // For Topology and DiscoverTopology.
load("jstests/libs/fixture_helpers.js");    // For 'FixtureHelpers'.
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a server?");

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type === Topology.kStandalone) {
    throw new Error("Can only analyze shard keys on a replica set or shard cluster, but got: " +
                    tojsononeline(topology));
}
if (topology.type === Topology.kReplicaSet) {
    throw new Error("This hook cannot run on a replica set");
}

/*
 * Returns the database name and collection name for a random user collection.
 */
function getRandomCollection() {
    const dbInfos =
        conn.getDBs(undefined /* driverSession */, {name: {$nin: ["local", "admin", "config"]}})
            .databases;
    if (dbInfos.length > 0) {
        const dbInfo = AnalyzeShardKeyUtil.getRandomElement(dbInfos);
        const collInfos = conn.getDB(dbInfo.name).getCollectionInfos({type: "collection"});

        if (collInfos.length > 0) {
            const collInfo = AnalyzeShardKeyUtil.getRandomElement(collInfos);
            return {dbName: dbInfo.name, collName: collInfo.name};
        }
    }
    return null;
}

/*
 * Returns all the shard keys that the analyzeShardKey command can analyze the cardinality and
 * frequency metrics for if the given index exists. For example, if the index is
 * {"a.x": 1, "b": hashed}, then the shard keys are {"a.x": 1}, {"a.x": hashed}, {"a.x": 1, "b": 1},
 * {"a.x": 1, "b": hashed}, {"a.x": hashed, b: 1}, and {"a.x": hashed, b: hashed}.
 */
function getSupportedShardKeys(indexKey) {
    const fieldNames = Object.keys(indexKey);
    let shardKeys = [];

    function generateShardKeys(currShardKey, currFieldIndex) {
        if (currFieldIndex > 0) {
            shardKeys.push(currShardKey);
        }
        if (currFieldIndex == fieldNames.length) {
            return;
        }
        const currFieldName = fieldNames[currFieldIndex];
        const nextFieldIndex = currFieldIndex + 1;
        generateShardKeys(Object.assign({}, currShardKey, {[currFieldName]: 1}), nextFieldIndex);
        if (!AnalyzeShardKeyUtil.isHashedKeyPattern(currShardKey)) {
            generateShardKeys(Object.assign({}, currShardKey, {[currFieldName]: "hashed"}),
                              nextFieldIndex);
        }
    }

    generateShardKeys({}, 0);
    return shardKeys;
}

/*
 * Generates a random shard key for the collection containing the given document.
 */
function generateRandomShardKey(doc) {
    const fieldNames = Object.keys(doc);
    let shardKey = {};
    let isHashed = false;  // There can only be one hashed field.

    for (let fieldName of fieldNames) {
        if (Math.random() > 0.5) {
            const isHashedField = !isHashed && (Math.random() > 0.5);
            shardKey[fieldName] = isHashedField ? "hashed" : 1;
            isHashed = isHashedField;
        }
    }
    if (Object.keys(shardKey).length == 0) {
        shardKey = {_id: 1};
    }
    return shardKey;
}

/*
 * Returns an array containing an object of the form {"numDocs": <integer>, "numBytes": <integer>}
 * for every shard in the cluster, where "numDocs" and "numBytes" are the number of documents and
 * total document size for the given collection.
 */
function getCollStatsOnAllShards(dbName, collName) {
    return FixtureHelpers
        .runCommandOnAllShards({
            db: conn.getDB(dbName),
            cmdObj: {
                aggregate: collName,
                pipeline: [
                    {$collStats: {storageStats: {}}},
                    {
                        $project: {
                            host: "$host",
                            numDocs: "$storageStats.count",
                            numBytes: "$storageStats.size"
                        }
                    }
                ],
                cursor: {}
            },
            primaryNodeOnly: true
        })
        .map((res) => {
            assert.commandWorked(res);
            return res.cursor.firstBatch[0];
        });
}

/*
 * Returns the most recently inserted config.sampledQueries document in the cluster.
 */
function getLatestSampleQueryDocument() {
    let latestDoc = null;
    FixtureHelpers
        .runCommandOnAllShards({
            db: conn.getDB("config"),
            cmdObj: {
                aggregate: "sampledQueries",
                pipeline: [{$sort: {expireAt: -1}}, {$limit: 1}],
                cursor: {}
            },
            primaryNodeOnly: true
        })
        .forEach((res) => {
            assert.commandWorked(res);
            if (res.cursor.firstBatch.length > 0) {
                const currentDoc = res.cursor.firstBatch[0];
                if (!latestDoc || bsonWoCompare(currentDoc.expireAt, latestDoc.expireAt) > 0) {
                    latestDoc = currentDoc;
                }
            }
        });
    return latestDoc;
}

/*
 * Runs the analyzeShardKey command to analyze the given shard key, and performs basic validation
 * of the resulting metrics.
 */
function analyzeShardKey(ns, shardKey, indexKey) {
    jsTest.log(`Analyzing shard keys ${tojsononeline({ns, shardKey, indexKey})}`);

    const res = conn.adminCommand({analyzeShardKey: ns, key: shardKey});

    if (res.code == ErrorCodes.BadValue || res.code == ErrorCodes.IllegalOperation ||
        res.code == ErrorCodes.NamespaceNotFound ||
        res.code == ErrorCodes.CommandNotSupportedOnView) {
        jsTest.log(
            `Failed to analyze the shard key because at least one of command options is invalid : ${
                tojsononeline(res)}`);
        return res;
    }
    if (res.code == 16746) {
        jsTest.log(`Failed to analyze the shard key because it contains an array index field: ${
            tojsononeline(res)}`);
        return res;
    }
    if (res.code == 4952606) {
        jsTest.log(`Failed to analyze the shard key because of its low cardinality: ${
            tojsononeline(res)}`);
        return res;
    }
    if (res.code == ErrorCodes.QueryPlanKilled) {
        jsTest.log(`Failed to analyze the shard key because the collection or the corresponding ` +
                   `index has been dropped or renamed: ${tojsononeline(res)}`);
        return res;
    }
    if (res.code == 640570) {
        jsTest.log(`Failed to analyze the shard key because the collection has been dropped and ` +
                   `that got detected through the shard version check ${tojsononeline(res)}`);
        return res;
    }
    if (res.code == 640571) {
        jsTest.log(`Failed to analyze the shard key because the collection has been dropped and ` +
                   `that got detected through the the database version check ` +
                   `${tojsononeline(res)}`);
        return res;
    }
    if (res.code == ErrorCodes.CollectionUUIDMismatch) {
        jsTest.log(`Failed to analyze the shard key because the collection has been recreated: ${
            tojsononeline(res)}`);
        return res;
    }

    assert.commandWorked(res);
    jsTest.log(`Finished analyzing the shard key: ${tojsononeline(res)}`);

    // The response should only contain the "numDocs" field if it also contains the fields about the
    // characteristics of the shard key (e.g. "numDistinctValues" and "mostCommonValues") since the
    // number of documents is just a supporting metric for those metrics.
    if (res.hasOwnProperty("numDocs")) {
        AnalyzeShardKeyUtil.assertContainKeyCharacteristicsMetrics(res);
    } else {
        AnalyzeShardKeyUtil.assertNotContainKeyCharacteristicsMetrics(res);
    }
    // The response should contain a "readDistribution" field and a "writeDistribution" field
    // whether or not there are sampled queries.
    AnalyzeShardKeyUtil.assertContainReadWriteDistributionMetrics(res);

    return res;
}

/*
 * Analyzes random shard keys for the given collection.
 */
function analyzeRandomShardKeys(dbName, collName) {
    const collInfos = conn.getDB(dbName).getCollectionInfos({type: "collection", name: collName});
    if (collInfos.length == 0) {
        // The collection no longer exists.
        return;
    }

    const ns = dbName + "." + collName;
    const coll = conn.getCollection(ns);
    jsTest.log("Analyzing random shard keys for the collection " +
               tojsononeline({ns, collInfo: collInfos[0]}));

    const doc = coll.findOne({});
    if (doc) {
        const shardKey = generateRandomShardKey(doc);
        if (!AnalyzeShardKeyUtil.isIdKeyPattern(shardKey)) {
            jsTest.log(`Analyzing a shard key that is likely to not have a corresponding index ${
                tojsononeline({shardKey, doc})}`);
            analyzeShardKey(ns, shardKey);
        }
    }

    const indexes = coll.getIndexes();
    if (indexes.length > 0) {
        const indexSpec = AnalyzeShardKeyUtil.getRandomElement(indexes);
        const shardKeys = getSupportedShardKeys(indexSpec.key);
        jsTest.log(`Analyzing shard keys that are likely to have a corresponding index ${
            tojsononeline({shardKeys, indexes})}`);
        // It is only "likely" because if the index above is not a hashed or b-tree index then it
        // can't be used for metrics calculation; additionally, the index may get dropped before or
        // while the analyzeShardKey command runs.
        for (let shardKey of shardKeys) {
            analyzeShardKey(ns, shardKey, indexSpec.key);
        }
    }
}

jsTest.log("Analyzing shard keys for a random collection");
const randomColl = getRandomCollection();
if (randomColl) {
    analyzeRandomShardKeys(randomColl.dbName, randomColl.collName);
}

jsTest.log(`Latest query sampling stats ${tojsononeline({
    "config.sampledQueriesStats": getCollStatsOnAllShards("config", "sampledQueries"),
    "config.sampledQueriesDiffStats": getCollStatsOnAllShards("config", "sampledQueriesDiff")
})}`);

jsTest.log("Analyzing shard keys for the collection for the latest sampled query");
// Such a collection is likely to still have more queries coming in. This gives us the test coverage
// for running the analyzeShardKey command while sampled queries are still being collected.
const latestSampledQueryDoc = getLatestSampleQueryDocument();
if (latestSampledQueryDoc) {
    const splits = latestSampledQueryDoc.ns.split(".");
    const dbName = splits[0];
    const collName = latestSampledQueryDoc.ns.substring(dbName.length + 1);
    analyzeRandomShardKeys(dbName, collName);
}
})();
