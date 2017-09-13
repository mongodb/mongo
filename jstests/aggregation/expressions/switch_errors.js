// SERVER-10689 introduced the $switch expression. In this file, we test the error cases of the
// expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var coll = db.switch;
    coll.drop();

    var pipeline = {"$project": {"output": {"$switch": "not an object"}}};
    assertErrorCode(coll, pipeline, 40060, "$switch requires an object as an argument.");

    pipeline = {"$project": {"output": {"$switch": {"branches": "not an array"}}}};
    assertErrorCode(coll, pipeline, 40061, "$switch requires 'branches' to be an array.");

    pipeline = {"$project": {"output": {"$switch": {"branches": ["not an object"]}}}};
    assertErrorCode(coll, pipeline, 40062, "$switch requires each branch to be an object.");

    pipeline = {"$project": {"output": {"$switch": {"branches": [{}]}}}};
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
        "$project":
            {"output": {"$switch": {"branches": [{"case": true, "then": false, "badKey": 1}]}}}
    };
    assertErrorCode(coll, pipeline, 40063, "$switch found a branch with an unknown argument");

    pipeline = {"$project": {"output": {"$switch": {"notAnArgument": 1}}}};
    assertErrorCode(coll, pipeline, 40067, "$switch found an unknown argument");

    pipeline = {"$project": {"output": {"$switch": {"branches": []}}}};
    assertErrorCode(coll, pipeline, 40068, "$switch requires at least one branch");

    pipeline = {"$project": {"output": {"$switch": {}}}};
    assertErrorCode(coll, pipeline, 40068, "$switch requires at least one branch");

    coll.insert({x: 1});
    pipeline = {
        "$project":
            {"output": {"$switch": {"branches": [{"case": {"$eq": ["$x", 0]}, "then": 1}]}}}
    };
    assertErrorCode(coll, pipeline, 40066, "$switch has no default and an input matched no case");
}());
