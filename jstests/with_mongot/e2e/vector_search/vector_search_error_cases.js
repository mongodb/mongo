/**
 * Test error conditions for the `$vectorSearch` aggregation pipeline stage.
 * E2E version of with_mongot/vector_search_mocked/vector_search_error_cases.js
 */

const collName = jsTestName();

const coll = db.getCollection(collName);
coll.insert({_id: 0});
coll.insert({_id: 1});
coll.insert({_id: 2});

function makeVectorSearchStage() {
    return {$vectorSearch: {queryVector: [], path: "x", numCandidates: 1, limit: 1}};
}

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

// $match cannot precede $vectorSearch.
assert.commandFailedWithCode(runPipeline([{$match: {}}, makeVectorSearchStage()]), 40602);

// $search and $vectorSearch are not allowed in the same pipeline.
assert.commandFailedWithCode(runPipeline([{$search: {}}, makeVectorSearchStage()]), 40602);
assert.commandFailedWithCode(runPipeline([makeVectorSearchStage(), {$search: {}}]), 40602);

// $vectorSearch must have a non-negative limit.
assert.commandFailedWithCode(
    runPipeline([{$vectorSearch: {queryVector: [], path: "x", numCandidates: 1, limit: -1}}]),
    [7912700, 65137 /** Extension error code */],
);

// $vectorSearch is not allowed in a sub-pipeline.
assert.commandFailedWithCode(
    runPipeline([{$lookup: {from: collName, pipeline: [makeVectorSearchStage()], as: "lookup1"}}]),
    51047,
);
assert.commandFailedWithCode(runPipeline([{$facet: {originalPipeline: [makeVectorSearchStage()]}}]), 40600);

// $vectorSearch does not support $SEARCH_META.
assert.commandFailedWithCode(
    runPipeline([makeVectorSearchStage(), {$project: {_id: 1, meta: "$$SEARCH_META"}}]),
    6347902,
);

// $vectorSearch cannot be used inside a transaction.
let session = db.getMongo().startSession({readConcern: {level: "local"}});
let sessionDb = session.getDatabase(jsTestName());
session.startTransaction();
assert.commandFailedWithCode(
    sessionDb.runCommand({aggregate: collName, pipeline: [makeVectorSearchStage()], cursor: {}}),
    ErrorCodes.OperationNotSupportedInTransaction,
);
session.endSession();

// $vectorSearch is not allowed in an update pipeline.
assert.commandFailedWithCode(
    db.runCommand({
        "findandmodify": collName,
        // Need a shardkey equality predicate to avoid having the command be implemented as a
        // transaction in sharded scenarios.
        "query": {_id: 0},
        "update": [makeVectorSearchStage()],
    }),
    ErrorCodes.InvalidOptions,
);

// Cleanup
coll.drop();
