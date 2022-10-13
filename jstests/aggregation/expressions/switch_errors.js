// SERVER-10689 introduced the $switch expression. In this file, we test the error cases of the
// expression.
load("jstests/aggregation/extras/utils.js");        // For assertErrorCode.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

(function() {
"use strict";

var coll = db.switch;
coll.drop();

var pipeline = {"$project": {"output": {"$switch": "not an object"}}};
assertErrorCode(coll, pipeline, 40060, "$switch requires an object as an argument.");

pipeline = {
    "$project": {"output": {"$switch": {"branches": "not an array"}}}
};
assertErrorCode(coll, pipeline, 40061, "$switch requires 'branches' to be an array.");

pipeline = {
    "$project": {"output": {"$switch": {"branches": ["not an object"]}}}
};
assertErrorCode(coll, pipeline, 40062, "$switch requires each branch to be an object.");

pipeline = {
    "$project": {"output": {"$switch": {"branches": [{}]}}}
};
assertErrorCode(coll, pipeline, 40064, "$switch requires each branch have a 'case'.");

pipeline = {
    "$project": {
        "output": {
            "$switch": {
                "branches": [{
                    "case": 1,
                }]
            }
        }
    }
};
assertErrorCode(coll, pipeline, 40065, "$switch requires each branch have a 'then'.");

pipeline = {
    "$project": {"output": {"$switch": {"branches": [{"case": true, "then": false, "badKey": 1}]}}}
};
assertErrorCode(coll, pipeline, 40063, "$switch found a branch with an unknown argument");

pipeline = {
    "$project": {"output": {"$switch": {"notAnArgument": 1}}}
};
assertErrorCode(coll, pipeline, 40067, "$switch found an unknown argument");

pipeline = {
    "$project": {"output": {"$switch": {"branches": []}}}
};
assertErrorCode(coll, pipeline, 40068, "$switch requires at least one branch");

pipeline = {
    "$project": {"output": {"$switch": {}}}
};
assertErrorCode(coll, pipeline, 40068, "$switch requires at least one branch");

assert.commandWorked(coll.insert({x: 1}));
pipeline = {
    "$project": {"output": {"$switch": {"branches": [{"case": {"$eq": ["$x", 0]}, "then": 1}]}}}
};
assertErrorCode(coll, pipeline, 40066, "$switch has no default and an input matched no case");

// This query was designed to reproduce SERVER-70190. The first branch of the $switch can be
// optimized away and the $ifNull can be optimized to 2. If the field "x" exists in the input
// document and is truthy, then the expression should return 2. Otherwise it should throw because no
// case statement matched and there is no "default" expression.
pipeline = [{
    $sortByCount: {
        $switch: {
            branches:
                [{case: {$literal: false}, then: 1}, {case: "$x", then: {$ifNull: [2, "$y"]}}]
        }
    }
}];
assert.eq([{"_id": 2, "count": 1}], coll.aggregate(pipeline).toArray());
assert.commandWorked(coll.remove({x: 1}));
assert.commandWorked(coll.insert({z: 1}));
assertErrorCode(coll, pipeline, 40066, "$switch has no default and an input matched no case");
}());
