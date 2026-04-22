/**
 * Regression test for SERVER-124974: a bug where $expr with $and containing $in predicates might
 * contain dangling pointers when the predicate is split during $match pushdown analysis.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: 10},
        {_id: 1, a: 2, b: 20},
        {_id: 2, a: 3, b: 30},
        {_id: 3, a: 4, b: 40},
    ]),
);

// $and fully dependent on the computed fields: no split on this side.
assert.sameMembers(
    coll
        .aggregate([
            {$addFields: {newF: {$add: ["$a", 1]}, newG: {$add: ["$b", 1]}}},
            {$match: {$expr: {$and: [{$in: ["$newF", [2, 4]]}, {$in: ["$newG", [11, 31]]}]}}},
            {$project: {_id: 1}},
        ])
        .toArray(),
    [{_id: 0}, {_id: 2}],
);

// $and fully independent of the computed fields: entire $expr can be pushed.
assert.sameMembers(
    coll
        .aggregate([
            {$addFields: {newF: {$add: ["$a", 1]}, newG: {$add: ["$b", 1]}}},
            {$match: {$expr: {$and: [{$in: ["$a", [1, 2, 3]]}, {$in: ["$b", [10, 20]]}]}}},
            {$project: {_id: 1}},
        ])
        .toArray(),
    [{_id: 0}, {_id: 1}],
);

// Case where the $expr is partially dependent: one predicate can be pushed down and the other cannot.
assert.sameMembers(
    coll
        .aggregate([
            {$addFields: {newF: {$add: ["$a", 1]}, newG: {$add: ["$b", 1]}}},
            {$match: {$expr: {$and: [{$in: ["$a", [1, 3]]}, {$in: ["$newG", [11, 31]]}]}}},
            {$project: {_id: 1}},
        ])
        .toArray(),
    [{_id: 0}, {_id: 2}],
);
