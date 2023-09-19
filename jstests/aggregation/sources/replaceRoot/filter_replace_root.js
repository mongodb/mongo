/**
 * Ensure that path expressions over the result of a $replaceRoot stage start from the new root and
 * not from the root of the original document.
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db.filter_replace_root;
coll.drop();

const docs = [
    {_id: 1, subdoc: {a: 10, subdoc: {a: 12}}},
    {_id: 2, subdoc: {a: 11, subdoc: {a: 11}}},
    {_id: 3, subdoc: {a: 12, subdoc: {a: 10}}}
];
assert.commandWorked(coll.insert(docs));

// Simple case where the old and new root both have a document with the same name.
let expected = docs.map(doc => doc.subdoc).filter(doc => doc.subdoc.a > 10);
let result =
    coll.aggregate(
            [{$replaceRoot: {newRoot: "$subdoc"}}, {$match: {$expr: {$gt: ["$subdoc.a", 10]}}}])
        .toArray();
assert(arrayEq(expected, result), {expected, result});

// This case also exercises an SBE edge case, where optimizing a $replaceRoot can introduce
// SBE-incompatible operators. If the planner does not observe the change in compatibility and tries
// to lower the expression to SBE anyway, it will crash the server.
expected = docs.map(doc => ({_id: doc._id, subdoc: doc.subdoc.a > doc.subdoc.subdoc.a}))
               .filter(doc => doc.subdoc);
result = coll.aggregate([
                 {
                     $replaceRoot: {
                         newRoot: {
                             _id: "$_id",
                             subdoc: {$and: [true, {$gt: ["$subdoc.a", "$subdoc.subdoc.a"]}]}
                         }
                     }
                 },
                 {$match: {subdoc: true}}
             ])
             .toArray();
assert(arrayEq(expected, result), {expected, result});
