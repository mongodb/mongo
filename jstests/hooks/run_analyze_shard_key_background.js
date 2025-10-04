import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {AnalyzeShardKeyUtil} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a server?");

const buildInfo = assert.commandWorked(db.runCommand({"buildInfo": 1}));
const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type === Topology.kStandalone) {
    throw new Error(
        "Can only analyze shard keys on a replica set or shard cluster, but got: " + tojsononeline(topology),
    );
}
if (topology.type === Topology.kReplicaSet) {
    throw new Error("This hook cannot run on a replica set");
}

/*
 * Returns the database name and collection name for a random user collection.
 */
function getRandomCollection() {
    const dbInfos = conn.getDBs(undefined /* driverSession */, {name: {$nin: ["local", "admin", "config"]}}).databases;
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
            generateShardKeys(Object.assign({}, currShardKey, {[currFieldName]: "hashed"}), nextFieldIndex);
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
    let isHashed = false; // There can only be one hashed field.

    for (let fieldName of fieldNames) {
        if (Math.random() > 0.5) {
            const isHashedField = !isHashed && Math.random() > 0.5;
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
    return FixtureHelpers.runCommandOnAllShards({
        db: conn.getDB(dbName),
        cmdObj: {
            aggregate: collName,
            pipeline: [
                {$collStats: {storageStats: {}}},
                {
                    $project: {
                        host: "$host",
                        numDocs: "$storageStats.count",
                        numBytes: "$storageStats.size",
                    },
                },
            ],
            cursor: {},
        },
        primaryNodeOnly: true,
    }).map((res) => {
        assert.commandWorked(res);
        return res.cursor.firstBatch[0];
    });
}

/*
 * Returns the most recently inserted config.sampledQueries document in the cluster.
 */
function getLatestSampleQueryDocument() {
    let latestDoc = null;
    FixtureHelpers.runCommandOnAllShards({
        db: conn.getDB("config"),
        cmdObj: {
            aggregate: "sampledQueries",
            pipeline: [{$sort: {expireAt: -1}}, {$limit: 1}],
            cursor: {},
        },
        primaryNodeOnly: true,
    }).forEach((res) => {
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
    const cmdObj = {analyzeShardKey: ns, key: shardKey};
    const rand = Math.random();
    if (rand < 0.25) {
        cmdObj.sampleRate = Math.random() * 0.5 + 0.5;
    } else if (rand < 0.5) {
        cmdObj.sampleSize = NumberLong(AnalyzeShardKeyUtil.getRandInteger(1000, 10000));
    }
    jsTest.log(`Analyzing shard keys ${tojsononeline({shardKey, indexKey, cmdObj})}`);
    const res = conn.adminCommand(cmdObj);

    if (
        res.code == ErrorCodes.BadValue ||
        res.code == ErrorCodes.IllegalOperation ||
        res.code == ErrorCodes.NamespaceNotFound ||
        res.code == ErrorCodes.CommandNotSupportedOnView
    ) {
        jsTest.log(
            `Failed to analyze the shard key because at least one of command options is invalid : ${tojsononeline(
                res,
            )}`,
        );
        return res;
    }
    if (res.code == 16746) {
        jsTest.log(`Failed to analyze the shard key because it contains an array index field: ${tojsononeline(res)}`);
        return res;
    }
    if (res.code == 4952606) {
        jsTest.log(`Failed to analyze the shard key because of its low cardinality: ${tojsononeline(res)}`);
        return res;
    }
    if (res.code == ErrorCodes.QueryPlanKilled) {
        jsTest.log(
            `Failed to analyze the shard key because the collection or the corresponding ` +
                `index has been dropped or renamed: ${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == 640570) {
        jsTest.log(
            `Failed to analyze the shard key because the collection has been dropped and ` +
                `that got detected through the shard version check ${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == 640571) {
        jsTest.log(
            `Failed to analyze the shard key because the collection has been dropped and ` +
                `that got detected through the the database version check ` +
                `${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == ErrorCodes.CollectionUUIDMismatch) {
        jsTest.log(`Failed to analyze the shard key because the collection has been recreated: ${tojsononeline(res)}`);
        return res;
    }
    if (res.code == 28799 || res.code == 4952606) {
        // (WT-8003) 28799 is the error that $sample throws when it fails to find a
        // non-duplicate document using a random cursor. 4952606 is the error that the sampling
        // based split policy throws if it fails to find the specified number of split points.
        print(`Failed to analyze the shard key due to duplicate keys returned by random cursor ${tojsononeline(res)}`);
        return res;
    }
    if (res.code == 7559401) {
        print(
            `Failed to analyze the shard key because one of the shards fetched the split ` +
                `point documents after the TTL deletions had started. ${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == 7588600) {
        print(
            `Failed to analyze the shard key because the document for one of the most common ` +
                `shard key values got deleted while the command was running. ${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == 7826501 || res.code == 7826502) {
        print(
            `Failed to analyze the shard key because $collStats indicates that the collection ` +
                `is empty. ${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == 7826505) {
        print(
            `Failed to analyze the shard key because the collection becomes empty during the ` +
                `step for calculating the monotonicity metrics. ${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == 7826506 || res.code == 7826507) {
        print(
            `Failed to analyze the shard key because the collection becomes empty during the ` +
                `step for calculating the cardinality and frequency metrics. ${tojsononeline(res)}`,
        );
        return res;
    }
    if (res.code == ErrorCodes.StaleConfig) {
        print(`Failed to analyze the shard key because it failed with a StaleConfig error. ` + `${tojsononeline(res)}`);
        return res;
    }
    if (res.code == ErrorCodes.NetworkInterfaceExceededTimeLimit && buildInfo.debug) {
        print(
            `Failed to analyze the shard key because network exceeded time limit in debug build. ` +
                `${tojsononeline(res)}`,
        );
        return res;
    }

    assert.commandWorked(res);
    jsTest.log(`Finished analyzing the shard key: ${tojsononeline(res)}`);

    if (res.hasOwnProperty("keyCharacteristics")) {
        AnalyzeShardKeyUtil.validateKeyCharacteristicsMetrics(res.keyCharacteristics);
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
    jsTest.log("Analyzing random shard keys for the collection " + tojsononeline({ns, collInfo: collInfos[0]}));

    let doc;
    try {
        doc = coll.findOne({});
    } catch (e) {
        if (e.code == ErrorCodes.QueryPlanKilled) {
            jsTest.log(
                `Skip analyzing shard keys for ${ns}` + `since the collection has been dropped: ${tojsononeline(e)}`,
            );
            return;
        }
        if (e.code == ErrorCodes.ObjectIsBusy) {
            jsTest.log(`Skip analyzing shard keys for ${ns}` + `since the storage is busy: ${tojsononeline(e)}`);
            return;
        }
        throw e;
    }

    if (doc) {
        const shardKey = generateRandomShardKey(doc);
        if (!AnalyzeShardKeyUtil.isIdKeyPattern(shardKey)) {
            jsTest.log(
                `Analyzing a shard key that is likely to not have a corresponding index ${tojsononeline({
                    shardKey,
                    doc,
                })}`,
            );
            analyzeShardKey(ns, shardKey);
        }
    }

    let indexes;
    // Catch any errors due to a collection potentially being dropped and recreated as a view.
    try {
        indexes = coll.getIndexes();
    } catch (err) {
        if (err.code == ErrorCodes.CommandNotSupportedOnView) {
            jsTestLog(
                "Skip analyzing shard keys for " +
                    ns +
                    " since the collection has been dropped and recreated as a view" +
                    tojson(err),
            );
            return;
        }
        throw err;
    }

    if (indexes.length > 0) {
        const indexSpec = AnalyzeShardKeyUtil.getRandomElement(indexes);
        const shardKeys = getSupportedShardKeys(indexSpec.key);
        jsTest.log(
            `Analyzing shard keys that are likely to have a corresponding index ${tojsononeline({shardKeys, indexes})}`,
        );
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

jsTest.log(
    `Latest query sampling stats ${tojsononeline({
        "config.sampledQueriesStats": getCollStatsOnAllShards("config", "sampledQueries"),
        "config.sampledQueriesDiffStats": getCollStatsOnAllShards("config", "sampledQueriesDiff"),
    })}`,
);

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
