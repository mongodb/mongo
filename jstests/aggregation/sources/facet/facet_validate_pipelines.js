// Test validation of $facet pipelines. Test that we correctly report the name of the stage not
// allowed inside $facet even if it is deeply nested in subpipelines.

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";

const coll = db.coll_facet;
const from = db.foreign_coll;

coll.drop();
assert.commandWorked(coll.insertMany([{_id: 0, a: 1}, {_id: 2, a: 5}]));

let pipeline = [{"$facet": {"x": [{"$facet": {"y": []}}]}}];

assertErrCodeAndErrMsgContains(
    coll, pipeline, 40600, "$facet is not allowed to be used within a $facet stage");

pipeline = [{
    "$facet": {
        "result": [
            {"$match": {"a": {"$exists": true}}},
            {
                "$lookup": {
                    "from": "foreign_coll",
                    "localField": "a",
                    "foreignField": "a",
                    "pipeline": [{
                        "$facet": {
                            "result": [
                                {"$match": {"c": {"$exists": true}}},
                                {"$project": {"a": 1, "b": 1}}
                            ]
                        }
                    }],
                    "as": "joined_docs"
                }
            }
        ]
    }
}];

assertErrCodeAndErrMsgContains(
    coll,
    pipeline,
    40600,
    "$facet inside of $lookup is not allowed to be used within a $facet stage");

pipeline = [{
    $facet: {
        "result_out": [{
            $unionWith:
                {coll: "foreign_coll", pipeline: [{$facet: {"result_in": [{$project: {"a": 1}}]}}]}
        }]
    }
}];

assertErrCodeAndErrMsgContains(
    coll,
    pipeline,
    40600,
    "$facet inside of $unionWith is not allowed to be used within a $facet stage");

pipeline = [{$facet: {"test": [{$lookup: {pipeline: [{$documents: [{"a": 1}]}], as: "foo"}}]}}];

assertErrCodeAndErrMsgContains(
    coll,
    pipeline,
    40600,
    "$documents inside of $lookup is not allowed to be used within a $facet stage");
