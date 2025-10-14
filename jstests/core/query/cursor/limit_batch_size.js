/**
 * Ensure that in trivial cases when batch size is equal to limit, an open cursor is not returned to the client.
 * @tags: [
 *   assumes_no_implicit_cursor_exhaustion,
 *   # TODO SERVER-111689 - remove this tag when isEOF() is supported on router.
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_83,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({index: i}));
}

function assertSingleBatch(cursor, batchSize, expectSingleBatch) {
    assert.eq(cursor.objsLeftInBatch(), batchSize);
    assert.eq(cursor.isClosed(), expectSingleBatch);
}

function testSingleBatchFind({query, sort, limit, batchSize, expectSingleBatch}) {
    const cursor = coll.find(query);
    if (sort !== undefined) {
        cursor.sort(sort);
    }
    if (limit !== undefined) {
        cursor.limit(limit);
    }
    cursor.batchSize(batchSize);
    assertSingleBatch(cursor, batchSize, expectSingleBatch);
}

function testSingleBatchAggregate(pipeline, batchSize, expectSingleBatch) {
    const cursor = coll.aggregate(pipeline, {cursor: {batchSize: batchSize}});
    assertSingleBatch(cursor, batchSize, expectSingleBatch);
}

testSingleBatchFind({query: {index: {$gte: 50}}, limit: 10, batchSize: 10, expectSingleBatch: true});
testSingleBatchAggregate([{$match: {index: {$gte: 50}}}, {$limit: 10}], 10, true);

testSingleBatchFind({query: {index: {$gte: 50}}, sort: {index: 1}, limit: 10, batchSize: 10, expectSingleBatch: true});
testSingleBatchAggregate([{$match: {index: {$gte: 50}}}, {$sort: {index: 1}}, {$limit: 10}], 10, true);

// There are exactly 10 matching documents, but there is no limit, so we must finish the scan to be sure.
testSingleBatchFind({query: {index: {$gte: 50}}, batchSize: 10, expectSingleBatch: false});
testSingleBatchAggregate([{$match: {index: {$lt: 10}}}], 10, false);

// $group "hides" $limit, so we can't guarantee a single batch.
testSingleBatchAggregate(
    [{$match: {index: {$gte: 50}}}, {$sort: {index: 1}}, {$limit: 10}, {$group: {_id: "$index", count: {$sum: 1}}}],
    10,
    false,
);
