/**
 * Basic tests for resharding for timeseries collection.
 * @tags: [
 *  featureFlagReshardingForTimeseries,
 *  requires_fcv_80,
 *  requires_timeseries,
 * ]
 */

import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";
import {createChunks, getShardNames} from "jstests/sharding/libs/sharding_util.js";

const shardNames = getShardNames(db);
const dbName = jsTestName();
const collName = "coll";
const ns = `${dbName}.${collName}`;
const mongos = db.getMongo();
const sourceCollection = mongos.getCollection(ns);
const sourceDB = sourceCollection.getDB();
const shardKeyPattern = {
    'meta.x': 1
};
const chunks = createChunks(shardNames, 'meta.x', -1, 6);
const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'meta'
};
const collOptions = {
    timeseries: timeseriesInfo
};

jsTestLog("Setting up the timeseries collection.");
assert.commandWorked(
    sourceDB.adminCommand({enableSharding: sourceDB.getName(), primaryShard: shardNames[0]}));

CreateShardedCollectionUtil.shardCollectionWithChunks(
    sourceCollection, shardKeyPattern, chunks, collOptions);

const bucketNss = dbName + ".system.buckets.coll";
let timeseriesCollDoc =
    mongos.getDB("config").getCollection("collections").findOne({_id: bucketNss});
assert.eq(timeseriesCollDoc.timeseriesFields.timeField, timeseriesInfo.timeField);
assert.eq(timeseriesCollDoc.timeseriesFields.metaField, timeseriesInfo.metaField);
assert.eq(timeseriesCollDoc.key, {"meta.x": 1});

const timeseriesCollection = mongos.getCollection(ns);
assert.commandWorked(timeseriesCollection.insert([
    {data: 1, ts: new Date(), meta: {x: -1, y: -1}},
    {data: 6, ts: new Date(), meta: {x: -1, y: -1}},
    {data: 3, ts: new Date(), meta: {x: -2, y: -2}},
    {data: 3, ts: new Date(), meta: {x: 4, y: 3}},
    {data: 9, ts: new Date(), meta: {x: 4, y: 3}},
    {data: 1, ts: new Date(), meta: {x: 5, y: 4}}
]));
assert.eq(6, mongos.getCollection(ns).countDocuments({}));

jsTestLog("Resharding the timeseries collection.");
const reshardCmdTest = new ReshardCollectionCmdTest({
    mongos,
    dbName,
    collName,
    numInitialDocs: 0,
    skipDirectShardChecks: true,
    skipCollectionSetup: true,
    timeseries: true
});

let newChunks = createChunks(shardNames, 'meta.y', -1, 4);
newChunks.forEach((_, idx) => {
    newChunks[idx]["recipientShardId"] = newChunks[idx]["shard"];
    delete newChunks[idx]["shard"];
});

reshardCmdTest.assertReshardCollOkWithPreset({
    reshardCollection: ns,
    key: {'meta.y': 1},
},
                                             newChunks);

let timeseriesCollDocPostResharding =
    mongos.getDB("config").getCollection("collections").findOne({_id: bucketNss});

// Resharding keeps timeseries fields.
assert.eq(timeseriesCollDocPostResharding.timeseriesFields.timeField, timeseriesInfo.timeField);
assert.eq(timeseriesCollDocPostResharding.timeseriesFields.metaField, timeseriesInfo.metaField);

// Resharding has updated shard key.
assert.eq(timeseriesCollDocPostResharding.key, {"meta.y": 1});

assert.eq(6, mongos.getCollection(ns).countDocuments({}));

sourceCollection.drop();
