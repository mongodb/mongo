/**
 * This test confirms that query stats store key fields for an aggregate command are properly nested
 * and none are missing when running a change stream query with $_passthroughToShard.
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */

import {
    runCommandAndValidateQueryStats,
} from "jstests/libs/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "coll";

// $_passthroughToShard is only possible on a sharded cluster.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    config: 1,
    rs: {nodes: 1},
    other: {
        mongosOptions: {
            setParameter: {
                internalQueryStatsRateLimit: -1,
            }
        }
    }
});

const sdb = st.s0.getDB(dbName);
assert.commandWorked(sdb.dropDatabase());

sdb.setProfilingLevel(0, -1);
st.shard0.getDB(dbName).setProfilingLevel(0, -1);

// Shard the relevant collections.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
// Shard the collection on {_id: 1}, split at {_id: 0} and move the empty upper chunk to
// shard1.
st.shardColl(collName, {_id: 1}, {_id: 0}, {_id: 0}, dbName);

const shardId = st.shard0.shardName;
let coll = sdb[collName];

const aggregateCommandObj = {
    aggregate: coll.getName(),
    pipeline: [{"$changeStream": {}}],
    allowDiskUse: false,
    cursor: {batchSize: 2},
    maxTimeMS: 50 * 1000,
    bypassDocumentValidation: false,
    readConcern: {level: "majority"},
    explain: true,
    collation: {locale: "en_US", strength: 2},
    hint: {"v": 1},
    comment: "",
    let : {},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
    $_passthroughToShard: {shard: shardId}
};

const queryShapeAggregateFields =
    ["cmdNs", "command", "pipeline", "allowDiskUse", "collation", "let"];

// The outer fields not nested inside queryShape.
const queryStatsAggregateKeyFields = [
    "queryShape",
    "cursor",
    "maxTimeMS",
    "bypassDocumentValidation",
    "comment",
    "apiDeprecationErrors",
    "apiVersion",
    "apiStrict",
    "collectionType",
    "client",
    "hint",
    "readConcern",
    "explain",
    "cursor.batchSize",
    "$_passthroughToShard",
    "$_passthroughToShard.shard"
];
assert.commandWorked(coll.createIndex({v: 1}));

runCommandAndValidateQueryStats({
    coll: coll,
    commandName: "aggregate",
    commandObj: aggregateCommandObj,
    shapeFields: queryShapeAggregateFields,
    keyFields: queryStatsAggregateKeyFields
});

st.stop();
