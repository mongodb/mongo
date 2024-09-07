/**
 * Test query knobs internalDocumentSourceCursorInitialBatchSize control DocumentSourceCursor batch
 * size, starting from a very small number and grows exponentially.
 */
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

const nDocs = 10;
for (let i = 1; i <= nDocs; i++) {
    assert.commandWorked(coll.insert({"_id": i}));
}

const getStats = function(explain) {
    assert(Array.isArray(explain), explain);
    assert(explain[0].hasOwnProperty('$cursor'), explain);
    return explain[0].$cursor.executionStats.nReturned;
};

const setCursorBatchSize = (size) => assert.commandWorked(
    db.adminCommand({setParameter: 1, internalDocumentSourceCursorInitialBatchSize: size}));

const nonOptStage = {
    $_internalInhibitOptimization: {}
};

{
    // Case where $limit cannot be pushed down to the executor.
    const pipeline = [{$match: {}}, nonOptStage, {$limit: 2}];

    // Make the initial batch size to be 0 (unlimited).
    setCursorBatchSize(0);

    // The documents are tiny, DocumentSourceCursor batches all documents.
    assert.eq(
        nDocs,
        getStats(assert.commandWorked(coll.explain('executionStats').aggregate(pipeline)).stages));

    // Make the initial batch size to be very small.
    setCursorBatchSize(1);

    // The executor returns a number of document that is close to what is needed, it is 3 instead of
    // 2 because the initial batch size is 1 and grows to 2 when the first batch is not enough.
    assert.eq(
        3,
        getStats(assert.commandWorked(coll.explain('executionStats').aggregate(pipeline)).stages));
}

// Make the initial batch size to be very small.
setCursorBatchSize(1);

{
    // Case when there are two $limit stages and only first one is pushed down and absorbed into
    // CanonicalQuery.
    const pipeline = [{$match: {}}, {$limit: 5}, nonOptStage, {$limit: 2}];
    // One $limit is pushed down, batching limitation won't work.
    assert.eq(
        5,
        getStats(assert.commandWorked(coll.explain('executionStats').aggregate(pipeline)).stages));
}

{
    // This test is for SBE only, it requires $group and $limit to be pushed down into query
    // executor.
    if (checkSbeFullyEnabled(db)) {
        // Case when there are two $limit stages and only first one is pushed down into cqPipeline
        // of CanonicalQuery.
        const pipeline =
            [{$group: {_id: "$_id", count: {$count: {}}}}, {$limit: 5}, nonOptStage, {$limit: 2}];
        // One $limit is pushed down, batching limitation won't work.
        assert.eq(
            5,
            getStats(
                assert.commandWorked(coll.explain('executionStats').aggregate(pipeline)).stages));
    }
}

{
    // Case when there is a $limit in sub-pipeline that can be pushed down, and a stricter $limit in
    // the main pipeline after $unionWith.
    const pipeline = [
        {$match: {_id: 1}},
        {$unionWith: {coll: coll.getName(), pipeline: [{$limit: 5}]}},
        {$limit: 2}
    ];
    const res = assert.commandWorked(coll.explain('executionStats').aggregate(pipeline));
    // $limit in sub-pipeline is pushed down, batching limitation won't work.
    assert.eq(5, getStats(res.stages[1].$unionWith.pipeline), res);
}

MongoRunner.stopMongod(conn);
