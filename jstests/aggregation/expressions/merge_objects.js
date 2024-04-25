// Tests for the $mergeObjects aggregation expression.
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let coll = db.merge_object_expr;
coll.drop();

// Test merging two objects together.
assert.commandWorked(coll.insert({_id: 0, subObject: {b: 1, c: 1}}));
let result = coll.aggregate([
                     {$match: {_id: 0}},
                     {$project: {mergedDocument: {$mergeObjects: ["$subObject", {d: 1}]}}}
                 ])
                 .toArray();
assert.eq(result, [{_id: 0, mergedDocument: {b: 1, c: 1, d: 1}}]);

// Test merging the root document with a new field.
assert.commandWorked(coll.insert({_id: 1, a: 0, b: 1}));
result = coll.aggregate([
                 {$match: {_id: 1}},
                 {$project: {mergedDocument: {$mergeObjects: ["$$ROOT", {newField: "newValue"}]}}}
             ])
             .toArray();
assert.eq(result, [{_id: 1, mergedDocument: {_id: 1, a: 0, b: 1, newField: "newValue"}}]);

// Test replacing a field in the root.
assert.commandWorked(coll.insert({_id: 2, a: 0, b: 1}));
result = coll.aggregate([
                 {$match: {_id: 2}},
                 {$project: {mergedDocument: {$mergeObjects: ["$$ROOT", {a: "newValue"}]}}}
             ])
             .toArray();
assert.eq(result, [{_id: 2, mergedDocument: {_id: 2, a: "newValue", b: 1}}]);

// Test overriding a document with root.
assert.commandWorked(coll.insert({_id: 3, a: 0, b: 1}));
result = coll.aggregate([
                 {$match: {_id: 3}},
                 {$project: {mergedDocument: {$mergeObjects: [{a: "defaultValue"}, "$$ROOT"]}}}
             ])
             .toArray();
assert.eq(result, [{_id: 3, mergedDocument: {a: 0, _id: 3, b: 1}}]);

// Test replacing root with merged document.
assert.commandWorked(coll.insert({_id: 4, a: 0, subObject: {b: 1, c: 2}}));
result = coll.aggregate([
                 {$match: {_id: 4}},
                 {$replaceRoot: {newRoot: {$mergeObjects: ["$$ROOT", "$subObject"]}}}
             ])
             .toArray();
assert.eq(result, [{_id: 4, a: 0, subObject: {b: 1, c: 2}, b: 1, c: 2}]);

// Test merging with an embedded object.
assert.commandWorked(coll.insert({_id: 5, subObject: {b: 1, c: 1}}));
result =
    coll.aggregate([
            {$match: {_id: 5}},
            {
                $project:
                    {mergedDocument: {$mergeObjects: ["$subObject", {subObject1: {d: 1}}, {e: 1}]}}
            }
        ])
        .toArray();
assert.eq(result, [{_id: 5, mergedDocument: {b: 1, c: 1, subObject1: {d: 1}, e: 1}}]);

// Test for errors on non-document types.
assert.commandWorked(coll.insert({_id: 6, a: "string"}));
assertErrorCode(
    coll,
    [{$match: {_id: 6}}, {$project: {mergedDocument: {$mergeObjects: ["$a", {a: "newString"}]}}}],
    40400);

assert.commandWorked(coll.insert({_id: 7, a: {b: 1}, c: 1}));
assertErrorCode(
    coll, [{$match: {_id: 7}}, {$project: {mergedDocument: {$mergeObjects: ["$a", "$c"]}}}], 40400);

// Test outputs with null values.
assert.commandWorked(coll.insert({_id: 8, a: {b: 1}}));
result =
    coll.aggregate(
            [{$match: {_id: 8}}, {$project: {mergedDocument: {$mergeObjects: ["$a", {b: null}]}}}])
        .toArray();
assert.eq(result, [{_id: 8, mergedDocument: {b: null}}]);

// Test output with undefined values.
assert.commandWorked(coll.insert({_id: 9, a: {b: 1}}));
result = coll.aggregate([
                 {$match: {_id: 9}},
                 {$project: {mergedDocument: {$mergeObjects: ["$a", {b: undefined}]}}}
             ])
             .toArray();
assert.eq(result, [{_id: 9, mergedDocument: {b: undefined}}]);
result = coll.aggregate({'$group': {_id: 0, m: {$mergeObjects: undefined}}}).toArray();
assert.eq(result, [{_id: 0, m: {}}]);

// Test output with missing values.
assert.commandWorked(coll.insert({_id: 10, a: {b: 1}}));
result = coll.aggregate([
                 {$match: {_id: 10}},
                 {$project: {mergedDocument: {$mergeObjects: ["$a", {b: "$nonExistentField"}]}}}
             ])
             .toArray();
assert.eq(result, [{_id: 10, mergedDocument: {b: 1}}]);

assert.commandWorked(coll.insert({_id: 11, a: {b: 1}}));
result =
    coll.aggregate(
            [{$match: {_id: 11}}, {$project: {mergedDocument: {$mergeObjects: ["$a", {b: ""}]}}}])
        .toArray();
assert.eq(result, [{_id: 11, mergedDocument: {b: ""}}]);

// Test outputs with empty values.
assert.commandWorked(coll.insert({_id: 12, b: 1, c: 1}));
result = coll.aggregate([{$match: {_id: 12}}, {$project: {mergedDocument: {$mergeObjects: [{}]}}}])
             .toArray();
assert.eq(result, [{_id: 12, mergedDocument: {}}]);

result =
    coll.aggregate([{$match: {_id: 12}}, {$project: {mergedDocument: {$mergeObjects: [{}, {}]}}}])
        .toArray();
assert.eq(result, [{_id: 12, mergedDocument: {}}]);

// Test merge within a $group stage.
assert.commandWorked(coll.insert({_id: 13, group: 1, obj: {}}));
assert.commandWorked(coll.insert({_id: 14, group: 1, obj: {a: 2, b: 2}}));
assert.commandWorked(coll.insert({_id: 15, group: 1, obj: {a: 1, c: 3}}));
assert.commandWorked(coll.insert({_id: 16, group: 2, obj: {a: 1, b: 1}}));
result = coll.aggregate([
                 {$match: {_id: {$in: [13, 14, 15, 16]}}},
                 {$sort: {_id: 1}},
                 {$group: {_id: "$group", mergedDocument: {$mergeObjects: "$obj"}}},
                 {$sort: {_id: 1}},
             ])
             .toArray();
assert.eq(result,
          [{_id: 1, mergedDocument: {a: 1, b: 2, c: 3}}, {_id: 2, mergedDocument: {a: 1, b: 1}}]);

// Test merge with $$REMOVE operator.
assert.commandWorked(coll.insert({_id: 17, a: {b: 2}}));
result = coll.aggregate([
                 {$match: {_id: 17}},
                 {$project: {mergedDocument: {$mergeObjects: ["$a", {b: "$$REMOVE"}]}}}
             ])
             .toArray();
assert.eq(result, [{_id: 17, mergedDocument: {b: 2}}]);

// Test that the behavior in $bucketAuto is the same as $group:
//   1. When all inputs end up in the same bucket, it's equivalent to {$group: {_id:1 ...}}.
{
    coll.drop();
    assert.commandWorked(coll.insert([
        {obj: {}},
        {obj: {a: 1}},
        {obj: {b: 2}},
        {obj: {subobj: {x: 1}}},
        {obj: {subobj: {y: 2}}},
        {obj: {a: 3}},
        {obj: {b: 4}},
    ]));
    const result = coll.aggregate([
                           {$sort: {_id: 1}},
                           {
                               $bucketAuto: {
                                   groupBy: "$no_such_field",
                                   buckets: 1,
                                   output: {result: {$mergeObjects: "$obj"}},
                               }
                           },
                       ])
                       .toArray();
    assert.sameMembers(result, [
        {
            _id: {min: null, max: null},
            // Last one wins, and subobjects are treated whole (not recursed into).
            result: {a: 3, b: 4, subobj: {y: 2}},
        },
    ]);
}
//   2. When inputs end up in separate buckets, $mergeObjects picks the "last" according to
//     the documents' position in the input--not according to the group key.
{
    coll.drop();
    assert.commandWorked(coll.insert([
        // With 8 docs and 2 buckets, we expect exactly 4 docs in each bucket.

        // In the first bucket, the documents happen to already be sorted by 'key'.
        // We expect the last value, a:4 to win.
        {key: 1, obj: {a: 1}},
        {key: 2, obj: {a: 2}},
        {key: 3, obj: {a: 3}},
        {key: 4, obj: {a: 4}},

        // In the second bucket, the documents are not already sorted by 'key'.
        // We expect the last one a:101 to win, as opposed to the largest/smallest key.
        {key: 102, obj: {a: 102}},
        {key: 103, obj: {a: 103}},
        {key: 100, obj: {a: 100}},
        {key: 101, obj: {a: 101}},
    ]));
    const result = coll.aggregate([
                           {$sort: {_id: 1}},
                           {
                               $bucketAuto: {
                                   groupBy: "$key",
                                   buckets: 2,
                                   output: {result: {$mergeObjects: "$obj"}},
                               }
                           },
                       ])
                       .toArray();
    assert.sameMembers(result, [
        // The upper bounds, '_id.max', are exclusive.
        {_id: {min: 1, max: 100}, result: {a: 4}},
        {_id: {min: 100, max: 103}, result: {a: 101}},
    ]);
}
