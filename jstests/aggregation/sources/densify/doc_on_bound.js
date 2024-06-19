/**
 * Test that densify works when a document exists on a bound.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */

import {anyEq, arrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];

function buildErrorString(found, expected) {
    return "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(found);
}

// Test a doc before the explicit range and one exactly on the bounds to ensure we densify from
// the correct value.
function testDocOnBoundsPartitioned() {
    coll.drop();

    let testDocs = [
        {"key": 1, "time": ISODate("2023-09-12T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-13T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-14T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-15T00:00:00.000Z"), "orig": true}
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {
            "$densify": {
                "field": "time",
                "partitionByFields": ["key"],
                "range": {
                    "step": 6,
                    "unit": "hour",
                    "bounds":
                        [ISODate("2023-09-13T00:00:00.000Z"), ISODate("2023-09-16T00:00:00.000Z")]
                }
            }
        },
        {$sort: {time: 1}},
        {$project: {_id: 0, time: 1, orig: 1}}

    ]);
    const resultArray = result.toArray();

    const expected = [
        {"time": ISODate("2023-09-12T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-13T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-13T06:00:00Z")},
        {"time": ISODate("2023-09-13T12:00:00Z")},
        {"time": ISODate("2023-09-13T18:00:00Z")},
        {"time": ISODate("2023-09-14T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-14T06:00:00Z")},
        {"time": ISODate("2023-09-14T12:00:00Z")},
        {"time": ISODate("2023-09-14T18:00:00Z")},
        {"time": ISODate("2023-09-15T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-15T06:00:00Z")},
        {"time": ISODate("2023-09-15T12:00:00Z")},
        {"time": ISODate("2023-09-15T18:00:00Z")}
    ];
    assert(arrayEq(resultArray, expected), buildErrorString(resultArray, expected));
}

// Same test as above, but no partition argument.
function testDocOnBoundsNotPartitioned() {
    coll.drop();

    let testDocs = [
        {"key": 1, "time": ISODate("2023-09-12T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-13T00:00:00.000Z"), "orig": true},
        {"key": 2, "time": ISODate("2023-09-13T00:00:10.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-14T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-15T00:00:00.000Z"), "orig": true}
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {
            "$densify": {
                "field": "time",
                "range": {
                    "step": 6,
                    "unit": "hour",
                    "bounds":
                        [ISODate("2023-09-13T00:00:00.000Z"), ISODate("2023-09-16T00:00:00.000Z")]
                }
            }
        },
        {$sort: {time: 1}},
        {$project: {_id: 0, time: 1, orig: 1}}

    ]);
    const resultArray = result.toArray();

    const expected = [
        {"time": ISODate("2023-09-12T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-13T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-13T00:00:10Z"), "orig": true},
        {"time": ISODate("2023-09-13T06:00:00Z")},
        {"time": ISODate("2023-09-13T12:00:00Z")},
        {"time": ISODate("2023-09-13T18:00:00Z")},
        {"time": ISODate("2023-09-14T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-14T06:00:00Z")},
        {"time": ISODate("2023-09-14T12:00:00Z")},
        {"time": ISODate("2023-09-14T18:00:00Z")},
        {"time": ISODate("2023-09-15T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-15T06:00:00Z")},
        {"time": ISODate("2023-09-15T12:00:00Z")},
        {"time": ISODate("2023-09-15T18:00:00Z")}
    ];
    assert(arrayEq(resultArray, expected), buildErrorString(resultArray, expected));
}

function testDocOnAndOffFullBound() {
    coll.drop();

    let testDocs = [
        {"key": 1, "time": ISODate("2023-09-12T00:00:00.000Z"), "orig": true},
        {"key": 2, "time": ISODate("2023-09-12T04:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-13T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-14T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-15T00:00:00.000Z"), "orig": true},
        {"key": 2, "time": ISODate("2023-09-15T18:00:00.000Z"), "orig": true}
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {
            "$densify": {
                "field": "time",
                "partitionByFields": ["key"],
                "range": {"step": 6, "unit": "hour", "bounds": "full"}
            }
        },
        {$sort: {time: 1}},
        {$project: {_id: 0, time: 1, orig: 1}}

    ]);
    const resultArray = result.toArray();

    const expected = [
        {"time": ISODate("2023-09-12T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-12T00:00:00Z")},
        {"time": ISODate("2023-09-12T04:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-12T06:00:00Z")},
        {"time": ISODate("2023-09-12T06:00:00Z")},
        {"time": ISODate("2023-09-12T12:00:00Z")},
        {"time": ISODate("2023-09-12T12:00:00Z")},
        {"time": ISODate("2023-09-12T18:00:00Z")},
        {"time": ISODate("2023-09-12T18:00:00Z")},
        {"time": ISODate("2023-09-13T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-13T00:00:00Z")},
        {"time": ISODate("2023-09-13T06:00:00Z")},
        {"time": ISODate("2023-09-13T06:00:00Z")},
        {"time": ISODate("2023-09-13T12:00:00Z")},
        {"time": ISODate("2023-09-13T12:00:00Z")},
        {"time": ISODate("2023-09-13T18:00:00Z")},
        {"time": ISODate("2023-09-13T18:00:00Z")},
        {"time": ISODate("2023-09-14T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-14T00:00:00Z")},
        {"time": ISODate("2023-09-14T06:00:00Z")},
        {"time": ISODate("2023-09-14T06:00:00Z")},
        {"time": ISODate("2023-09-14T12:00:00Z")},
        {"time": ISODate("2023-09-14T12:00:00Z")},
        {"time": ISODate("2023-09-14T18:00:00Z")},
        {"time": ISODate("2023-09-14T18:00:00Z")},
        {"time": ISODate("2023-09-15T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-15T00:00:00Z")},
        {"time": ISODate("2023-09-15T06:00:00Z")},
        {"time": ISODate("2023-09-15T06:00:00Z")},
        {"time": ISODate("2023-09-15T12:00:00Z")},
        {"time": ISODate("2023-09-15T12:00:00Z")},
        {"time": ISODate("2023-09-15T18:00:00Z")},
        {"time": ISODate("2023-09-15T18:00:00Z"), "orig": true}
    ];
    assert(arrayEq(resultArray, expected), buildErrorString(resultArray, expected));
}

function testFullNoPartition() {
    coll.drop();

    let testDocs = [
        {"key": 1, "time": ISODate("2023-09-13T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-14T00:00:00.000Z"), "orig": true},
        {"key": 1, "time": ISODate("2023-09-15T00:00:00.000Z"), "orig": true},
    ];
    assert.commandWorked(coll.insert(testDocs));

    let result = coll.aggregate([
        {
            "$densify": {
                "field": "time",
                "partitionByFields": ["key"],
                "range": {"step": 6, "unit": "hour", "bounds": "full"}
            }
        },
        {$sort: {time: 1}},
        {$project: {_id: 0, time: 1, orig: 1}}

    ]);
    const resultArray = result.toArray();

    const expected = [
        {"time": ISODate("2023-09-13T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-13T06:00:00Z")},
        {"time": ISODate("2023-09-13T12:00:00Z")},
        {"time": ISODate("2023-09-13T18:00:00Z")},
        {"time": ISODate("2023-09-14T00:00:00Z"), "orig": true},
        {"time": ISODate("2023-09-14T06:00:00Z")},
        {"time": ISODate("2023-09-14T12:00:00Z")},
        {"time": ISODate("2023-09-14T18:00:00Z")},
        {"time": ISODate("2023-09-15T00:00:00Z"), "orig": true},
    ];
    assert(arrayEq(resultArray, expected), buildErrorString(resultArray, expected));
}
testDocOnBoundsPartitioned();
testDocOnBoundsNotPartitioned();
testDocOnAndOffFullBound();
testFullNoPartition();