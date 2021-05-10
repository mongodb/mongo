/**
 * Test that estimate of the total data size sorted when spilling to disk is reasonable.
 *
 * This test was originally designed to reproduce SERVER-53760.
 */
(function() {
"use strict";
load('jstests/libs/analyze_plan.js');  // For 'getAggPlanStage()'.

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const collName = jsTestName();
const coll = db[collName];
coll.drop();
const numDocs = 5;
const bigStrLen = numDocs * 40;
const arrLen = numDocs * 40;

// To reproduce SERVER-53760, we need to create a collection with N documents, where each document
// is sizable and consists of an array field, called `data`. Then, if we pass the collection through
// a pipeline consisting of a `$unwind` (on `data`) followed by `$sort`, the documents in the output
// of `$unwind` all share the same backing BSON in the original collection. Next, if the sorter does
// not calculate the appropriate size of the document (by discarding the parts of backing BSON not
// used by each document in the output of `$unwind`), the size approximation can be way bigger than
// the actual amount, which can result in (unnecessarily) opening too many files (and even running
// out of the number of allowed open files for some operating systems). In this example, it'd be a
// factor of 100x.
const docs = [];
let totalSize = 0;
const str = "a".repeat(bigStrLen);
for (let i = 0; i < numDocs; ++i) {
    let doc = {_id: i, foo: i * 2};
    let arr = [];
    for (let j = 0; j < arrLen; ++j) {
        arr.push({bigString: str, uniqueValue: j});
    }

    doc["data"] = arr;
    docs.push(doc);
    totalSize += Object.bsonsize(doc);
}

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 5000}));
assert.commandWorked(coll.insert(docs));

function createPipeline(collection) {
    return collection.aggregate(
        [
            {$unwind: "$data"},
            {$sort: {'_id': -1, 'data.val': -1}},
            {$limit: 900},
            {$group: {_id: 0, sumTop900UniqueValues: {$sum: '$data.uniqueValue'}}}
        ],
        {allowDiskUse: true});
}

const explain = createPipeline(coll.explain("executionStats"));
const sortStages = getAggPlanStages(explain, "$sort");

assert.eq(sortStages.length, 1, explain);
const sort = sortStages[0];
const dataBytesSorted = sort["totalDataSizeSortedBytesEstimate"];

// The total data size sorted is no greater than 3x the total size of all documents sorted.
assert.lt(dataBytesSorted, 3 * totalSize, explain);

assert.eq(createPipeline(coll).toArray(), [{_id: 0, sumTop900UniqueValues: 84550}], explain);

MongoRunner.stopMongod(conn);
})();
