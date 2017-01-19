// Tests for the $objectToArray aggregation expression.
(function() {
    "use strict";

    // For assertErrorCode().
    load("jstests/aggregation/extras/utils.js");

    let coll = db.object_to_array_expr;
    coll.drop();

    let object_to_array_expr = {$project: {expanded: {$objectToArray: "$subDoc"}}};

    // $objectToArray correctly converts a document to an array of key-value pairs.
    assert.writeOK(coll.insert({_id: 0, subDoc: {"a": 1, "b": 2, "c": "foo"}}));
    let result = coll.aggregate([{$match: {_id: 0}}, object_to_array_expr]).toArray();
    assert.eq(
        result,
        [{_id: 0, expanded: [{"k": "a", "v": 1}, {"k": "b", "v": 2}, {"k": "c", "v": "foo"}]}]);

    assert.writeOK(coll.insert({_id: 1, subDoc: {"y": []}}));
    result = coll.aggregate([{$match: {_id: 1}}, object_to_array_expr]).toArray();
    assert.eq(result, [{_id: 1, expanded: [{"k": "y", "v": []}]}]);

    assert.writeOK(coll.insert({_id: 2, subDoc: {"a": 1, "b": {"d": "string"}, "c": [1, 2]}}));
    result = coll.aggregate([{$match: {_id: 2}}, object_to_array_expr]).toArray();
    assert.eq(
        result, [{
            _id: 2,
            expanded:
                [{"k": "a", "v": 1}, {"k": "b", "v": {"d": "string"}}, {"k": "c", "v": [1, 2]}]
        }]);

    assert.writeOK(coll.insert({_id: 3, subDoc: {}}));
    result = coll.aggregate([{$match: {_id: 3}}, object_to_array_expr]).toArray();
    assert.eq(result, [{_id: 3, expanded: []}]);

    // Turns to array from the root of the document.
    assert.writeOK(coll.insert({_id: 4, "a": 1, "b": 2, "c": 3}));
    result =
        coll.aggregate([{$match: {_id: 4}}, {$project: {document: {$objectToArray: "$$ROOT"}}}])
            .toArray();
    assert.eq(result, [
        {
          _id: 4,
          document:
              [{"k": "_id", "v": 4}, {"k": "a", "v": 1}, {"k": "b", "v": 2}, {"k": "c", "v": 3}]
        }
    ]);

    assert.writeOK(coll.insert({_id: 5, "date": ISODate("2017-01-24T00:00:00")}));
    result = coll.aggregate([
                     {$match: {_id: 5}},
                     {$project: {document: {$objectToArray: {dayOfWeek: {$dayOfWeek: "$date"}}}}}
                 ])
                 .toArray();
    assert.eq(result, [{_id: 5, document: [{"k": "dayOfWeek", "v": 3}]}]);

    // $objectToArray errors on non-document types.
    assert.writeOK(coll.insert({_id: 6, subDoc: "string"}));
    assertErrorCode(coll, [{$match: {_id: 6}}, object_to_array_expr], 40390);

    assert.writeOK(coll.insert({_id: 7, subDoc: ObjectId()}));
    assertErrorCode(coll, [{$match: {_id: 7}}, object_to_array_expr], 40390);

    assert.writeOK(coll.insert({_id: 8, subDoc: NumberLong(0)}));
    assertErrorCode(coll, [{$match: {_id: 8}}, object_to_array_expr], 40390);

    assert.writeOK(coll.insert({_id: 9, subDoc: []}));
    assertErrorCode(coll, [{$match: {_id: 9}}, object_to_array_expr], 40390);

    assert.writeOK(coll.insert({_id: 10, subDoc: [0]}));
    assertErrorCode(coll, [{$match: {_id: 10}}, object_to_array_expr], 40390);

    assert.writeOK(coll.insert({_id: 11, subDoc: ["string"]}));
    assertErrorCode(coll, [{$match: {_id: 11}}, object_to_array_expr], 40390);

    assert.writeOK(coll.insert({_id: 12, subDoc: [{"a": "b"}]}));
    assertErrorCode(coll, [{$match: {_id: 12}}, object_to_array_expr], 40390);

    assert.writeOK(coll.insert({_id: 13, subDoc: NaN}));
    assertErrorCode(coll, [{$match: {_id: 13}}, object_to_array_expr], 40390);

    // $objectToArray outputs null on null-ish types.
    assert.writeOK(coll.insert({_id: 14, subDoc: null}));
    result = coll.aggregate([{$match: {_id: 14}}, object_to_array_expr]).toArray();
    assert.eq(result, [{_id: 14, expanded: null}]);

    assert.writeOK(coll.insert({_id: 15, subDoc: undefined}));
    result = coll.aggregate([{$match: {_id: 15}}, object_to_array_expr]).toArray();
    assert.eq(result, [{_id: 15, expanded: null}]);

    assert.writeOK(coll.insert({_id: 16}));
    result = coll.aggregate([{$match: {_id: 16}}, object_to_array_expr]).toArray();
    assert.eq(result, [{_id: 16, expanded: null}]);
}());
