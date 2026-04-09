/**
 * Regression test for the 8.0.20 mongos crash in Pipeline::reattachToOperationContext()
 * triggered by getMore calls on a sharded $search pipeline.
 *
 * The failing query shape in production was:
 *   $search -> $limit -> $project(paginationToken: {$meta: "searchSequenceToken"}, ...)
 *           -> $group($first/$last) -> $sort -> $limit
 *
 * running on mongos with a small batchSize that forces multiple getMore calls. The $sort stage
 * buffers all $group output before sorting, causing $group to call dispose() upstream (including
 * on DocumentSourceSetVariableFromSubPipeline, which sets _subPipeline=null). On the first
 * getMore, reattachToOperationContext() unconditionally dereferences the null _subPipeline,
 * crashing with SIGSEGV at address 0x28 (Pipeline::pCtx field offset).
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: jsTestName(),
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    mongos: 1,
    other: {
        rsOptions: {setParameter: {enableTestCommands: 1}},
    },
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testColl = testDB[collName];
const collNS = testColl.getFullName();
testColl.drop();

// Insert documents matching the production schema: execution_id + ingested fields.
// Use enough docs to generate multiple result groups and force several getMore roundtrips.
const numExecIds = 5;
const docs = [];
for (let i = 0; i < 20; i++) {
    docs.push({
        _id: i,
        execution_id: `exec${i % numExecIds}`,
        ingested: new Date(2024, 0, i + 1),
    });
}
assert.commandWorked(testColl.insertMany(docs));

// Shard the collection: _id < 10 on shard0, _id >= 10 on shard1.
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 11});

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

// The exact production query shape reported in the crash.
const mongotQuery = {};
const protocolVersion = NumberInt(1);

// $meta: "searchSequenceToken" in the projection causes MongoDB to set
// requiresSearchSequenceToken: true in the mongot command, and mongot returns per-document tokens.
const cursorOptions = {
    requiresSearchSequenceToken: true
};

// The meta cursor batch returned by each shard mongot. Simulates a mongot count response.
// $$SEARCH_META.count.total will resolve to null in this test, but the value doesn't matter —
// we only need the reference to $$SEARCH_META to trigger DocumentSourceSetVariableFromSubPipeline
// creation, which is the stage that crashes on reattachToOperationContext.
const metaCursorBatch = [{count: {total: 20}}];

// Shard 0 returns _id 0–9 with sequence tokens.
const shard0Docs = [
    {_id: 0, $searchScore: 1.00, $searchSequenceToken: "aaa0000=="},
    {_id: 1, $searchScore: 0.98, $searchSequenceToken: "aaa0001=="},
    {_id: 2, $searchScore: 0.96, $searchSequenceToken: "aaa0002=="},
    {_id: 3, $searchScore: 0.94, $searchSequenceToken: "aaa0003=="},
    {_id: 4, $searchScore: 0.92, $searchSequenceToken: "aaa0004=="},
    {_id: 5, $searchScore: 0.90, $searchSequenceToken: "aaa0005=="},
    {_id: 6, $searchScore: 0.88, $searchSequenceToken: "aaa0006=="},
    {_id: 7, $searchScore: 0.86, $searchSequenceToken: "aaa0007=="},
    {_id: 8, $searchScore: 0.84, $searchSequenceToken: "aaa0008=="},
    {_id: 9, $searchScore: 0.82, $searchSequenceToken: "aaa0009=="},
];

// Shard 1 returns _id 10–19 with sequence tokens.
const shard1Docs = [
    {_id: 10, $searchScore: 0.81, $searchSequenceToken: "bbb0010=="},
    {_id: 11, $searchScore: 0.79, $searchSequenceToken: "bbb0011=="},
    {_id: 12, $searchScore: 0.77, $searchSequenceToken: "bbb0012=="},
    {_id: 13, $searchScore: 0.75, $searchSequenceToken: "bbb0013=="},
    {_id: 14, $searchScore: 0.73, $searchSequenceToken: "bbb0014=="},
    {_id: 15, $searchScore: 0.71, $searchSequenceToken: "bbb0015=="},
    {_id: 16, $searchScore: 0.69, $searchSequenceToken: "bbb0016=="},
    {_id: 17, $searchScore: 0.67, $searchSequenceToken: "bbb0017=="},
    {_id: 18, $searchScore: 0.65, $searchSequenceToken: "bbb0018=="},
    {_id: 19, $searchScore: 0.63, $searchSequenceToken: "bbb0019=="},
];

const cursorId = NumberLong(123);
const secondCursorId = NumberLong(1124);
const responseOk = 1;

// Wire up shard 0 mongotmock.
const expectedCmd0 = mongotCommandForQuery({
    query: mongotQuery,
    collName,
    db: dbName,
    collectionUUID: collUUID0,
    protocolVersion,
    cursorOptions,
});
const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
s0Mongot.setMockResponses(
    [{
        expectedCommand: expectedCmd0,
        response: mongotMultiCursorResponseForBatch(
            shard0Docs, NumberLong(0), metaCursorBatch, NumberLong(0), collNS, responseOk),
    }],
    cursorId,
    secondCursorId);

// Wire up shard 1 mongotmock.
const expectedCmd1 = mongotCommandForQuery({
    query: mongotQuery,
    collName,
    db: dbName,
    collectionUUID: collUUID1,
    protocolVersion,
    cursorOptions,
});
const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
s1Mongot.setMockResponses(
    [{
        expectedCommand: expectedCmd1,
        response: mongotMultiCursorResponseForBatch(
            shard1Docs, NumberLong(0), metaCursorBatch, NumberLong(0), collNS, responseOk),
    }],
    cursorId,
    secondCursorId);

// Wire up the mongos planShardedSearch response (needed once per query).
mockPlanShardedSearchResponse(collName, mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);

// Production pipeline shape — exactly as reported in the crash report.
//
// The reference to $$SEARCH_META.count.total is the critical trigger: it sets
// _queryReferencesSearchMeta=true in DocumentSourceSearch::optimizeAt(), which causes
// DocumentSourceSetVariableFromSubPipeline to be inserted into the merge pipeline in
// distributedPlanLogic(). That stage is the one that crashes.
//
// Crash mechanism in 8.0.20:
// 1. $sort (downstream of $group) buffers all $group output before sorting, calling
//    $group::doGetNext() until EOF.
// 2. When $group returns EOF, it calls dispose() on itself, which propagates upstream:
//    $project -> $limit -> DocumentSourceSetVariableFromSubPipeline.
// 3. DocumentSourceSetVariableFromSubPipeline::doDispose() calls _subPipeline.reset(),
//    setting _subPipeline to nullptr.
// 4. $sort then outputs its first result to the client (cursor saved with _subPipeline==null).
// 5. On the first getMore, checkOutCursor() calls reattachToOperationContext(), reaching
//    DocumentSourceSetVariableFromSubPipeline::reattachToOperationContext().
// 6. In 8.0.20, this unconditionally calls _subPipeline->reattachToOperationContext(opCtx)
//    without a null check. With _subPipeline==null, a SIGSEGV occurs.
const pipeline = [
    {$search: mongotQuery},
    {
        $project: {
            execution_id: 1,
            ingested: 1,
            paginationToken: {$meta: "searchSequenceToken"},
            total: "$$SEARCH_META.count.total",
            _id: 0,
        }
    },
    {
        $group: {
            _id: "$execution_id",
            ingested: {$first: "$ingested"},
            paginationToken: {$last: "$paginationToken"},
            total: {$first: "$total"},
        }
    },
    {$sort: {ingested: -1, _id: 1}},
    {$limit: 5},
];

const cmd = testDB.runCommand({
    aggregate: collName,
    pipeline,
    cursor: {batchSize: 1},
});
assert.commandWorked(cmd, "Initial aggregate should succeed");
jsTestLog(`Initial batch: ${tojson(cmd.cursor.firstBatch)} cursorId=${cmd.cursor.id}`);

// If cursorId is 0, all results came back in the first batch — no getMore issued, no crash.
assert.neq(
    cmd.cursor.id,
    0,
    "Expected cursor to require getMore (cursorId should be non-zero). " +
        "If this fails, the pipeline returned all results in one batch and won't reproduce the crash.");

// Explicitly call getMore — this triggers the reattachToOperationContext crash in 8.0.20.
jsTestLog("About to call getMore — this should crash in 8.0.20 due to null _subPipeline");
const getMoreResult = testDB.runCommand({
    getMore: cmd.cursor.id,
    collection: collName,
    batchSize: 1,
});
jsTestLog(`getMore result: ${tojson(getMoreResult)}`);

// If we reach here, the crash didn't happen (running on a fixed version).
// Exhaust the cursor.
let allResultsLength = cmd.cursor.firstBatch.length;
let curBatch = getMoreResult.cursor ? getMoreResult.cursor.nextBatch : [];
allResultsLength += curBatch.length;
let remainingCursorId = getMoreResult.cursor ? getMoreResult.cursor.id : 0;
while (remainingCursorId != 0) {
    const r = testDB.runCommand({getMore: remainingCursorId, collection: collName, batchSize: 1});
    assert.commandWorked(r);
    allResultsLength += r.cursor.nextBatch.length;
    remainingCursorId = r.cursor.id;
}

jsTestLog(`Query returned ${allResultsLength} documents — no crash on getMore.`);
assert.gt(allResultsLength, 0, "Expected at least one grouped result");

stWithMock.stop();
