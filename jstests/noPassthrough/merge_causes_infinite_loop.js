/**
 * Test that exposes the Halloween problem.
 *
 * The Halloween problem describes the potential for a document to be visited more than once
 * following an update operation that changes its physical location.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
const db = conn.getDB("merge_causes_infinite_loop");
const coll = db.getCollection("merge_causes_infinite_loop");
const out = db.getCollection("merge_causes_infinite_loop_out");
coll.drop();
out.drop();

const nDocs = 50;
// We seed the documents with large values for a. This enables the pipeline that exposes the
// halloween problem to overflow and fail more quickly.
const largeNum = 1000 * 1000 * 1000;

// We set internalQueryExecYieldPeriodMS to 1 ms to have query execution yield as often as
// possible.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 1}));

// Insert documents.
// Note that the largeArray field is included to force documents to be written to disk and not
// simply be updated in the cache. This is crucial to exposing the halloween problem as the
// physical location of each document needs to change for each document to visited and updated
// multiple times.
var bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, a: i * largeNum, largeArray: (new Array(1024 * 1024).join("a"))});
}
assert.commandWorked(bulk.execute());

// Build an index over a, the field to be updated, so that updates will push modified documents
// forward in the index when outputting to the collection being aggregated.
assert.commandWorked(coll.createIndex({a: 1}));

// Returns a pipeline which outputs to the specified collection.
function pipeline(outColl) {
    return [
        {$match: {a: {$gt: 0}}},
        {$merge: {into: outColl, whenMatched: [{$addFields: {a: {$multiply: ["$a", 2]}}}]}}
    ];
}

const differentCollPipeline = pipeline(out.getName());
const sameCollPipeline = pipeline(coll.getName());

// Outputting the result of this pipeline to a different collection will not time out nor will any
// of the computed values overflow.
assert.commandWorked(db.runCommand(
    {aggregate: coll.getName(), pipeline: differentCollPipeline, cursor: {}, maxTimeMS: 2500}));

// Because this pipeline writes to the collection being aggregated, it will cause documents to be
// updated and pushed forward indefinitely. This will cause the computed values to eventually
// overflow.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: sameCollPipeline, cursor: {}}), 31109);
MongoRunner.stopMongod(conn);
}());
