/**
 * Tests $graphLookup does not attempt to spill on mongos. The leading $sort splits the pipeline,
 * so $graphLookup lands on the merging half.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const setParameter = {internalDocumentSourceGraphLookupMaxMemoryBytes: 32 * 1024};
const st = new ShardingTest({
    shards: 2,
    other: {mongosOptions: {setParameter}, rsOptions: {setParameter}},
});

const mongosDB = st.s.getDB(dbName);
const sourceColl = mongosDB.source;
const foreignColl = mongosDB.foreign;

assert.commandWorked(mongosDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongosDB.adminCommand({shardCollection: sourceColl.getFullName(), key: {_id: 1}}));
assert.commandWorked(mongosDB.adminCommand({split: sourceColl.getFullName(), middle: {_id: 0}}));
assert.commandWorked(
    mongosDB.adminCommand({
        moveChunk: sourceColl.getFullName(),
        find: {_id: 0},
        to: st.shard1.shardName,
    }),
);

const numSourceDocs = 10;
const numForeignDocs = 200;
const padding = "a".repeat(1024);
assert.commandWorked(sourceColl.insert(Array.from({length: numSourceDocs}, (_, i) => ({_id: i - 5, startPoint: 0}))));
assert.commandWorked(
    foreignColl.insert(Array.from({length: numForeignDocs}, (_, i) => ({_id: i, next: i + 1, padding}))),
);

const pipeline = [
    {$sort: {visited: 1}},
    {
        $graphLookup: {
            from: foreignColl.getName(),
            startWith: "$startPoint",
            connectFromField: "next",
            connectToField: "_id",
            as: "visited",
        },
    },
];

// Only an explicit 'allowDiskUse' moves the merge off the router
for (const [options, mergeType] of [
    [{allowDiskUse: true}, "anyShard"],
    [{}, "router"],
    [{allowDiskUse: false}, "router"],
]) {
    const explain = sourceColl.explain().aggregate(pipeline, options);
    assert.eq(explain.mergeType, mergeType, explain);

    const res = mongosDB.runCommand(Object.assign({aggregate: sourceColl.getName(), pipeline, cursor: {}}, options));
    if (mergeType === "router") {
        assert.commandFailedWithCode(res, ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
        continue;
    }

    assert.commandWorked(res);
    const docs = new DBCommandCursor(mongosDB, res).toArray();
    assert.eq(numSourceDocs, docs.length, docs);
    for (const doc of docs) {
        assert.eq(numForeignDocs, doc.visited.length, doc);
    }
}

st.stop();
