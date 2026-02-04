/**
 * Test that the SBE engine correctly handles the case where the unwind field is
 * an empty array and we're projecting the array index.
 *
 * @tags: [
 *    # Aggregate is not supported in multi-document transactions.
 *    does_not_support_transactions,
 *    requires_non_retryable_writes,
 * ]
 */
import {resultsEq} from "jstests/aggregation/extras/utils.js";
db.c.drop();
db.c.insert({_id: 0, array: []});
db.c.createIndex({array: -1});
const pipeline = [
    {"$sort": {array: -1}},
    {
        "$unwind": {
            path: "$array",
            preserveNullAndEmptyArrays: true,
            includeArrayIndex: "m",
        },
    },
    {"$project": {b: "$m"}},
];
const results = db.c.aggregate(pipeline).toArray();
assert(resultsEq([{"_id": 0, "b": null}], results));

db.c.deleteMany({});
db.c.insert({
    _id: 0,
    "m": {"m2": 0},
});
const pipeline2 = [
    {"$unwind": {"path": "$a", "preserveNullAndEmptyArrays": true, "includeArrayIndex": "a"}},
    {"$addFields": {"a": 0}},
];
assert(resultsEq([{_id: 0, "m": {"m2": 0}, "a": 0}], db.c.aggregate(pipeline2).toArray()));

db.c.deleteMany({});
db.c.insert({"_id": 105, "m": {"m1": 0}, "a": 0});
const pipeline3 = [
    {
        "$project": {
            "_id": 0,
            "a": 1,
        },
    },
    {
        "$unwind": {
            "path": "$m.m1",
            "preserveNullAndEmptyArrays": true,
            "includeArrayIndex": "a",
        },
    },
    {
        "$project": {
            "_id": 0,
            "a": 0,
        },
    },
];
assert(resultsEq([{}], db.c.aggregate(pipeline3).toArray()));

db.c.deleteMany({});
db.c.insertMany([
    {
        "_id": 244,
        "t": ISODate("1970-01-01T00:00:00Z"),
        "m": {
            "m1": NumberInt(0),
            "m2": NumberInt(0),
        },
        "array": [],
        "a": NumberInt(0),
        "b": NumberInt(0),
    },
]);
const pipeline4 = [
    {
        "$project": {
            "_id": 0,
            "a": 0,
        },
    },
    {
        "$unwind": {
            "path": "$array",
            "preserveNullAndEmptyArrays": true,
        },
    },
    {
        "$addFields": {
            "a": "$a",
        },
    },
];
assert(
    resultsEq(
        [
            {
                "t": ISODate("1970-01-01T00:00:00Z"),
                "m": {
                    "m1": 0,
                    "m2": 0,
                },
                "b": 0,
            },
        ],
        db.c.aggregate(pipeline4).toArray(),
    ),
);
