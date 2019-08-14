// Tests for $arrayToObject aggregation expression.
(function() {
"use strict";

// For assertErrorCode().
load("jstests/aggregation/extras/utils.js");

let coll = db.array_to_object_expr;
coll.drop();

// Write one document so that the aggregations which use $const produce a result.
assert.commandWorked(coll.insert({_id: "sentinel", a: 1}));

/*
 * Check that the collapsed, object form of 'expanded' (which is computed using $arrayToObject)
 * matches our expectation.
 */
function assertCollapsed(expanded, expectedCollapsed) {
    const result =
        coll.aggregate([{$project: {collapsed: {$arrayToObject: {$const: expanded}}}}, {$limit: 1}])
            .toArray();
    assert.eq(result, [{_id: "sentinel", collapsed: expectedCollapsed}]);
}

/*
 * Check that $arrayToObject on the given value produces the expected error.
 */
function assertPipelineErrors(expanded, errorCode) {
    assertErrorCode(coll,
                    [{$project: {collapsed: {$arrayToObject: {$const: expanded}}}}, {$limit: 1}],
                    errorCode);
}

// $arrayToObject correctly converts a key-value pairs to an object.
assertCollapsed([["price", 24], ["item", "apple"]], {"price": 24, "item": "apple"});
assertCollapsed([{"k": "price", "v": 24}, {"k": "item", "v": "apple"}],
                {"price": 24, "item": "apple"});
// If duplicate field names are in the array, $arrayToObject should use value from the last one.
assertCollapsed([{"k": "price", "v": 24}, {"k": "price", "v": 100}], {"price": 100});
assertCollapsed([["price", 24], ["price", 100]], {"price": 100});

assertCollapsed([["price", 24], ["item", "apple"]], {"price": 24, "item": "apple"});
assertCollapsed([], {});

assertCollapsed(null, null);
assertCollapsed(undefined, null);
assertCollapsed([{"k": "price", "v": null}], {"price": null});
assertCollapsed([{"k": "price", "v": undefined}], {"price": undefined});
// Need to manually check the case where 'expanded' is not in the document.
assert.commandWorked(coll.insert({_id: "missing-expanded-field"}));
const result = coll.aggregate([
                       {$match: {_id: "missing-expanded-field"}},
                       {$project: {collapsed: {$arrayToObject: "$expanded"}}}
                   ])
                   .toArray();
assert.eq(result, [{_id: "missing-expanded-field", collapsed: null}]);

assertPipelineErrors([{"k": "price", "v": 24}, ["item", "apple"]], 40391);
assertPipelineErrors([["item", "apple"], {"k": "price", "v": 24}], 40396);
assertPipelineErrors("string", 40386);
assertPipelineErrors(ObjectId(), 40386);
assertPipelineErrors(NumberLong(0), 40386);
assertPipelineErrors([0], 40398);
assertPipelineErrors([["missing_value"]], 40397);
assertPipelineErrors([[321, 12]], 40395);
assertPipelineErrors([["key", "value", "offset"]], 40397);
assertPipelineErrors({y: []}, 40386);
assertPipelineErrors([{y: "x", x: "y"}], 40393);
assertPipelineErrors([{k: "missing"}], 40392);
assertPipelineErrors([{k: 24, v: "string"}], 40394);
assertPipelineErrors([{k: null, v: "nullKey"}], 40394);
assertPipelineErrors([{k: undefined, v: "undefinedKey"}], 40394);
assertPipelineErrors([{y: "ignored", k: "item", v: "pear"}], 40392);
assertPipelineErrors(NaN, 40386);
}());
