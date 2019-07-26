// Basic integration tests for $replaceRoot and its alias $replaceWith.
(function() {
"use strict";

const coll = db.replaceWith_use_cases;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, comments: [{user_id: "x", comment: "foo"}, {user_id: "y", comment: "bar"}]},
    {_id: 1, comments: [{user_id: "y", comment: "bar again"}]}
]));

// Test computing the most frequent commenters using $replaceRoot.
let pipeline =
    [{$unwind: "$comments"}, {$replaceRoot: {newRoot: "$comments"}}, {$sortByCount: "$user_id"}];
const expectedResults = [{_id: "y", count: 2}, {_id: "x", count: 1}];
assert.eq(coll.aggregate(pipeline).toArray(), expectedResults);

// Test the same thing but using the $replaceWith alias.
pipeline = [{$unwind: "$comments"}, {$replaceWith: "$comments"}, {$sortByCount: "$user_id"}];
assert.eq(coll.aggregate(pipeline).toArray(), expectedResults);
}());
