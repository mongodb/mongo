/**
 * Tests that an $elemMatch-$or query is evaluated correctly. Designed to reproduce SERVER-33005 and
 * SERVER-38164.
 */
(function() {
"use strict";

const coll = db.elemmatch_or_pushdown;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: 1, b: [{c: 4}]}));
assert.commandWorked(coll.insert({_id: 1, a: 2, b: [{c: 4}]}));
assert.commandWorked(coll.insert({_id: 2, a: 2, b: [{c: 5}]}));
assert.commandWorked(coll.insert({_id: 3, a: 1, b: [{c: 5}]}));
assert.commandWorked(coll.insert({_id: 4, a: 1, b: [{c: 6}]}));
assert.commandWorked(coll.insert({_id: 5, a: 1, b: [{c: 7}]}));
assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));

assert.eq(coll.find({a: 1, b: {$elemMatch: {$or: [{c: 4}, {c: 5}]}}}).sort({_id: 1}).toArray(),
          [{_id: 0, a: 1, b: [{c: 4}]}, {_id: 3, a: 1, b: [{c: 5}]}]);
assert.eq(coll.find({a: 1, $or: [{a: 2}, {b: {$elemMatch: {$or: [{c: 4}, {c: 5}]}}}]})
              .sort({_id: 1})
              .toArray(),
          [{_id: 0, a: 1, b: [{c: 4}]}, {_id: 3, a: 1, b: [{c: 5}]}]);

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 5, b: [{c: [{f: 8}], d: 6}]}));
assert.commandWorked(coll.insert({_id: 1, a: 4, b: [{c: [{f: 8}], d: 6}]}));
assert.commandWorked(coll.insert({_id: 2, a: 5, b: [{c: [{f: 8}], d: 7}]}));
assert.commandWorked(coll.insert({_id: 3, a: 4, b: [{c: [{f: 9}], d: 6}]}));
assert.commandWorked(coll.insert({_id: 4, a: 5, b: [{c: [{f: 8}], e: 7}]}));
assert.commandWorked(coll.insert({_id: 5, a: 4, b: [{c: [{f: 8}], e: 7}]}));
assert.commandWorked(coll.insert({_id: 6, a: 5, b: [{c: [{f: 8}], e: 8}]}));
assert.commandWorked(coll.insert({_id: 7, a: 5, b: [{c: [{f: 9}], e: 7}]}));
assert.commandWorked(coll.createIndex({"b.d": 1, "b.c.f": 1}));
assert.commandWorked(coll.createIndex({"b.e": 1, "b.c.f": 1}));

assert.eq(coll.find({a: 5, b: {$elemMatch: {c: {$elemMatch: {f: 8}}, $or: [{d: 6}, {e: 7}]}}})
              .sort({_id: 1})
              .toArray(),
          [{_id: 0, a: 5, b: [{c: [{f: 8}], d: 6}]}, {_id: 4, a: 5, b: [{c: [{f: 8}], e: 7}]}]);

// Test that $not predicates in $elemMatch can be pushed into an $or sibling of the $elemMatch.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, arr: [{a: 0, b: 2}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 1, arr: [{a: 1, b: 2}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 2, arr: [{a: 0, b: 3}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 3, arr: [{a: 1, b: 3}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 4, arr: [{a: 0, b: 2}], c: 6, d: 7}));
assert.commandWorked(coll.insert({_id: 5, arr: [{a: 1, b: 2}], c: 6, d: 7}));
assert.commandWorked(coll.insert({_id: 6, arr: [{a: 0, b: 3}], c: 6, d: 7}));
assert.commandWorked(coll.insert({_id: 7, arr: [{a: 1, b: 3}], c: 6, d: 7}));

const keyPattern = {
    "arr.a": 1,
    "arr.b": 1,
    c: 1,
    d: 1
};
assert.commandWorked(coll.createIndex(keyPattern));

const elemMatchOr = {
    arr: {$elemMatch: {a: {$ne: 1}, $or: [{b: 2}, {b: 3}]}},
    $or: [
        {c: 4, d: 5},
        {c: 6, d: 7},
    ],
};

// Confirm that we get the same results using the index and a COLLSCAN.
for (let hint of [keyPattern, {$natural: 1}]) {
    assert.eq(coll.find(elemMatchOr, {_id: 1}).sort({_id: 1}).hint(hint).toArray(),
              [{_id: 0}, {_id: 2}, {_id: 4}, {_id: 6}]);

    assert.eq(
        coll.aggregate(
                [
                    {$match: {arr: {$elemMatch: {a: {$ne: 1}}}, $or: [{c: 4, d: 5}, {c: 6, d: 7}]}},
                    {$project: {_id: 1}},
                    {$sort: {_id: 1}}
                ],
                {hint: hint})
            .toArray(),
        [{_id: 0}, {_id: 2}, {_id: 4}, {_id: 6}]);
}
}());
