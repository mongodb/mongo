// Tests for the $mergeObjects aggregation expression.
(function() {
    "use strict";

    // For assertErrorCode().
    load("jstests/aggregation/extras/utils.js");

    let coll = db.merge_object_expr;
    coll.drop();

    // Test merging two objects together.
    assert.writeOK(coll.insert({_id: 0, subObject: {b: 1, c: 1}}));
    let result = coll.aggregate([
                         {$match: {_id: 0}},
                         {$project: {mergedDocument: {$mergeObjects: ["$subObject", {d: 1}]}}}
                     ])
                     .toArray();
    assert.eq(result, [{_id: 0, mergedDocument: {b: 1, c: 1, d: 1}}]);

    // Test merging the root document with a new field.
    assert.writeOK(coll.insert({_id: 1, a: 0, b: 1}));
    result =
        coll.aggregate([
                {$match: {_id: 1}},
                {$project: {mergedDocument: {$mergeObjects: ["$$ROOT", {newField: "newValue"}]}}}
            ])
            .toArray();
    assert.eq(result, [{_id: 1, mergedDocument: {_id: 1, a: 0, b: 1, newField: "newValue"}}]);

    // Test replacing a field in the root.
    assert.writeOK(coll.insert({_id: 2, a: 0, b: 1}));
    result = coll.aggregate([
                     {$match: {_id: 2}},
                     {$project: {mergedDocument: {$mergeObjects: ["$$ROOT", {a: "newValue"}]}}}
                 ])
                 .toArray();
    assert.eq(result, [{_id: 2, mergedDocument: {_id: 2, a: "newValue", b: 1}}]);

    // Test overriding a document with root.
    assert.writeOK(coll.insert({_id: 3, a: 0, b: 1}));
    result =
        coll.aggregate([
                {$match: {_id: 3}},
                {$project: {mergedDocument: {$mergeObjects: [{a: "defaultValue"}, "$$ROOT"]}}}
            ])
            .toArray();
    assert.eq(result, [{_id: 3, mergedDocument: {a: 0, _id: 3, b: 1}}]);

    // Test replacing root with merged document.
    assert.writeOK(coll.insert({_id: 4, a: 0, subObject: {b: 1, c: 2}}));
    result = coll.aggregate([
                     {$match: {_id: 4}},
                     {$replaceRoot: {newRoot: {$mergeObjects: ["$$ROOT", "$subObject"]}}}
                 ])
                 .toArray();
    assert.eq(result, [{_id: 4, a: 0, subObject: {b: 1, c: 2}, b: 1, c: 2}]);

    // Test merging with an embedded object.
    assert.writeOK(coll.insert({_id: 5, subObject: {b: 1, c: 1}}));
    result = coll.aggregate([
                     {$match: {_id: 5}},
                     {
                       $project: {
                           mergedDocument:
                               {$mergeObjects: ["$subObject", {subObject1: {d: 1}}, {e: 1}]}
                       }
                     }
                 ])
                 .toArray();
    assert.eq(result, [{_id: 5, mergedDocument: {b: 1, c: 1, subObject1: {d: 1}, e: 1}}]);

    // Test for errors on non-document types.
    assert.writeOK(coll.insert({_id: 6, a: "string"}));
    assertErrorCode(coll,
                    [
                      {$match: {_id: 6}},
                      {$project: {mergedDocument: {$mergeObjects: ["$a", {a: "newString"}]}}}
                    ],
                    40400);

    assert.writeOK(coll.insert({_id: 7, a: {b: 1}, c: 1}));
    assertErrorCode(
        coll,
        [{$match: {_id: 7}}, {$project: {mergedDocument: {$mergeObjects: ["$a", "$c"]}}}],
        40400);

    // Test outputs with null values.
    assert.writeOK(coll.insert({_id: 8, a: {b: 1}}));
    result = coll.aggregate([
                     {$match: {_id: 8}},
                     {$project: {mergedDocument: {$mergeObjects: ["$a", {b: null}]}}}
                 ])
                 .toArray();
    assert.eq(result, [{_id: 8, mergedDocument: {b: null}}]);

    // Test output with undefined values.
    assert.writeOK(coll.insert({_id: 9, a: {b: 1}}));
    result = coll.aggregate([
                     {$match: {_id: 9}},
                     {$project: {mergedDocument: {$mergeObjects: ["$a", {b: undefined}]}}}
                 ])
                 .toArray();
    assert.eq(result, [{_id: 9, mergedDocument: {b: undefined}}]);

    // Test output with missing values.
    assert.writeOK(coll.insert({_id: 10, a: {b: 1}}));
    result =
        coll.aggregate([
                {$match: {_id: 10}},
                {$project: {mergedDocument: {$mergeObjects: ["$a", {b: "$nonExistentField"}]}}}
            ])
            .toArray();
    assert.eq(result, [{_id: 10, mergedDocument: {b: 1}}]);

    assert.writeOK(coll.insert({_id: 11, a: {b: 1}}));
    result = coll.aggregate([
                     {$match: {_id: 11}},
                     {$project: {mergedDocument: {$mergeObjects: ["$a", {b: ""}]}}}
                 ])
                 .toArray();
    assert.eq(result, [{_id: 11, mergedDocument: {b: ""}}]);

    // Test outputs with empty values.
    assert.writeOK(coll.insert({_id: 12, b: 1, c: 1}));
    result =
        coll.aggregate([{$match: {_id: 12}}, {$project: {mergedDocument: {$mergeObjects: [{}]}}}])
            .toArray();
    assert.eq(result, [{_id: 12, mergedDocument: {}}]);

    result = coll.aggregate(
                     [{$match: {_id: 12}}, {$project: {mergedDocument: {$mergeObjects: [{}, {}]}}}])
                 .toArray();
    assert.eq(result, [{_id: 12, mergedDocument: {}}]);

    // Test merge within a $group stage.
    assert.writeOK(coll.insert({_id: 13, group: 1, obj: {}}));
    assert.writeOK(coll.insert({_id: 14, group: 1, obj: {a: 2, b: 2}}));
    assert.writeOK(coll.insert({_id: 15, group: 1, obj: {a: 1, c: 3}}));
    assert.writeOK(coll.insert({_id: 16, group: 2, obj: {a: 1, b: 1}}));
    result = coll.aggregate([
                     {$match: {_id: {$in: [13, 14, 15, 16]}}},
                     {$sort: {_id: 1}},
                     {$group: {_id: "$group", mergedDocument: {$mergeObjects: "$obj"}}},
                     {$sort: {_id: 1}},
                 ])
                 .toArray();
    assert.eq(
        result,
        [{_id: 1, mergedDocument: {a: 1, b: 2, c: 3}}, {_id: 2, mergedDocument: {a: 1, b: 1}}]);

    // Test merge with $$REMOVE operator.
    assert.writeOK(coll.insert({_id: 17, a: {b: 2}}));
    result = coll.aggregate([
                     {$match: {_id: 17}},
                     {$project: {mergedDocument: {$mergeObjects: ["$a", {b: "$$REMOVE"}]}}}
                 ])
                 .toArray();
    assert.eq(result, [{_id: 17, mergedDocument: {b: 2}}]);

}());
