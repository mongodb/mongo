/**
 * Test a particular nested $and/$or query. This test was designed to reproduce SERVER-106983, a bug
 * in which the plan enumerator only generates one plan for this query, but the plan oscillates
 * between two sets of bounds.
 */
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {resetCollection} from "jstests/query_golden/libs/utils.js";

const coll = db.or_pred_pushdown_coll;

const docs = [
    {
        "_id": 187,
        "t": ISODate("1970-01-01T00:00:00Z"),
        "m": {"m1": NumberInt(0), "m2": NumberInt(0)},
        "array": [], // This makes the index we create below multikey.
        "a": NumberInt(0),
        "b": NumberInt(0),
    },
    {
        "_id": 83,
        "t": ISODate("1970-01-01T00:00:00Z"),
        "m": {"m1": NumberInt(0), "m2": ISODate("1970-01-01T00:00:00Z")},
        "array": "",
        "a": NumberInt(0),
        "b": NumberInt(0),
    },
];

const pipeline = [
    {
        "$match": {
            "$or": [{"t": {"$exists": true}}, {"_id": 0, "a": 0}],
            "$and": [{"array": {"$nin": [0]}}, {"array": {"$eq": ""}}],
        },
    },
];

resetCollection(coll, docs, /* indexes */ [{t: 1, array: 1}]);
outputAggregationPlanAndResults(coll, pipeline);
