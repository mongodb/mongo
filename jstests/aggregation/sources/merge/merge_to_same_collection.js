/**
 * Tests that $merge does not fail when the target collection is the aggregation collection.
 *
 * @tags: [assumes_unsharded_collection]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // for assertArrayEq()

const coll = db.name;
coll.drop();

const nDocs = 3;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}
const pipeline = [
    {$match: {a: {$lt: 1}}},
    {
        $merge: {
            into: coll.getName(),
            whenMatched: [{$addFields: {a: {$add: ["$a", 3]}}}],
            whenNotMatched: "insert"
        }
    }
];

assert.doesNotThrow(() => coll.aggregate(pipeline));

assertArrayEq(
    {actual: coll.find().toArray(), expected: [{_id: 0, a: 3}, {_id: 1, a: 1}, {_id: 2, a: 2}]});
}());
