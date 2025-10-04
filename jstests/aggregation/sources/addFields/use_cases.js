/**
 * $addFields can be used to add fixed and computed fields to documents while preserving the
 * original document. Verify that using $addFields and adding computed fields in a $project yield
 * the same result.
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";

const collName = jsTest.name();
const coll = db.getCollection(collName);
coll.drop();

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({"_id": i, "2i": i * 2, "3i": i * 3}));
}

// Add the minimum, maximum, and average temperatures, and make sure that doing the same
// with addFields yields the correct answer.
// First compute with $project, since we know all the fields in this document.
let projectPipe = [
    {
        $project: {
            "2i": 1,
            "3i": 1,
            "6i^2": {"$multiply": ["$2i", "$3i"]},
            // _id is implicitly included.
        },
    },
];
let correct = coll.aggregate(projectPipe).toArray();

// Then compute the same results using $addFields.
let addFieldsPipe = [
    {
        $addFields: {
            "6i^2": {"$multiply": ["$2i", "$3i"]},
            // All other fields are implicitly included.
        },
    },
];
let addFieldsResult = coll.aggregate(addFieldsPipe).toArray();

// Then assert they are the same.
assert(
    arrayEq(addFieldsResult, correct),
    "$addFields does not work the same as a $project with computed and included fields",
);

// $addFields with an empty spec is allowed and should be treated as a no-op
let addFieldsEmptySpecPipe = [{$addFields: {}}];

assert(
    arrayEq(coll.aggregate(addFieldsEmptySpecPipe).toArray(), coll.aggregate().toArray()),
    "$addFields with empty spec did not result in no-op",
);

// Checks that new fields don't attempt to read slots yet to be produced by the same stage.
let addFieldResult1 = coll
    .aggregate([{$addFields: {"obj": "$2i", "result": {"$multiply": ["$obj", "$3i"]}}}, {$sort: {obj: 1}}, {$limit: 2}])
    .toArray();
assert.eq(2, addFieldResult1.length, addFieldResult1);
