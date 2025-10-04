/**
 * Test that exposes the Halloween problem.
 *
 * The Halloween problem describes the potential for a document to be visited more than once
 * following an update operation that changes its physical location. The purpose of this test is
 * to show that this behavior can be encountered when running a $merge aggregation which writes
 * to the collection being read from.
 */
const conn = MongoRunner.runMongod();
const db = conn.getDB("merge_causes_infinite_loop");
const coll = db.getCollection("merge_causes_infinite_loop");
const out = db.getCollection("merge_causes_infinite_loop_out");
coll.drop();
out.drop();

const nDocs = 50;
// We seed the documents with large values for a.
const largeNum = 1000 * 1000 * 1000;

// We set internalQueryExecYieldPeriodMS to 1 ms to have query execution yield as often as
// possible.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 1}));

// Insert documents into both collections. We populate the output collection to verify that
// updates behave as expected when the source collection isn't the same as the target collection.
//
// Note that the largeArray field is included to force documents to be written to disk and not
// simply be updated in the cache. This is crucial to exposing the halloween problem as the
// physical location of each document needs to change for each document to be visited and updated
// multiple times.
function insertDocuments(collObject) {
    const bulk = collObject.initializeUnorderedBulkOp();
    for (let i = 1; i < nDocs; i++) {
        bulk.insert({_id: i, a: i * largeNum, largeArray: "a".repeat(1024 * 1024)});
    }
    assert.commandWorked(bulk.execute());
}

insertDocuments(coll);
insertDocuments(out);

// Build an index over a, the field to be updated, so that updates will push modified documents
// forward in the index when outputting to the collection being aggregated.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(out.createIndex({a: 1}));

// Returns a pipeline which outputs to the specified collection.
function pipeline(outColl) {
    return [
        {$match: {a: {$gt: 0}}},
        {$merge: {into: outColl, whenMatched: [{$addFields: {a: {$multiply: ["$a", 2]}}}]}},
    ];
}

const differentCollPipeline = pipeline(out.getName());
const sameCollPipeline = pipeline(coll.getName());

// Targeting a collection that is not the collection being aggregated over will result in each
// document's value of 'a' being updated exactly once.
assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: differentCollPipeline, cursor: {}}));

// Filter out 'largeArray' as we are only interested in verifying the value of "a" in each
// document.
const diffCollResult = out.find({}, {largeArray: 0}).toArray();

for (const doc of diffCollResult) {
    assert(doc.hasOwnProperty("a"), doc);
    const expectedVal = doc["_id"] * 2 * largeNum;
    assert.eq(doc["a"], expectedVal, doc);
}

// Targeting the same collection that is being aggregated over will result in some documents' value
// of 'a' being updated multiple times.
assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: sameCollPipeline, cursor: {}}));

const sameCollResult = coll.find({}, {largeArray: 0}).toArray();

// At least one document in our collection should have been updated multiple times.
let foundDocumentUpdatedMultipleTimes = false;
for (const doc of sameCollResult) {
    assert(doc.hasOwnProperty("a"), doc);
    const expectedVal = doc["_id"] * 2 * largeNum;
    const actualVal = doc["a"];

    // If we find a mismatch, it must be the case that 'actualVal' is at least twice as large as
    // 'expectedVal'. This means that the $multiply expression was applied multiple times to the
    // same document.
    if (actualVal !== expectedVal && actualVal >= 2 * expectedVal) {
        foundDocumentUpdatedMultipleTimes = true;
        break;
    }
}

assert(
    foundDocumentUpdatedMultipleTimes,
    "All documents were updated exactly once, which is unexpected. Contents of the collection" +
        " being aggregated over and merged into: " +
        tojson(sameCollResult),
);

MongoRunner.stopMongod(conn);
