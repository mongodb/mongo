/**
 * Verifies that we correcly process overrding local fields by foreign documents.
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const docs = [
    {_id: "first", a: 1, b: 1},
    {_id: "second", a: 1, b: 2},
];

const config = {
    setParameter: {
        internalEnableJoinOptimization: true,
    },
};

const conn = MongoRunner.runMongod(config);

const db = conn.getDB(jsTestName());

db.coll.drop();
assert.commandWorked(db.coll.insertMany(docs));

const pipeline = [
    {$lookup: {from: "coll", localField: "_id", foreignField: "_id", as: "_id"}},
    {$unwind: "$_id"},
    {$lookup: {from: "coll", localField: "a", foreignField: "b", as: "a"}},
    {$unwind: "$a"},
    {$lookup: {from: "coll", localField: "b", foreignField: "b", as: "b"}},
    {$unwind: "$b"},
];

const actual = db.coll.aggregate(pipeline).toArray();
MongoRunner.stopMongod(conn);

const expected = [
    {
        "_id": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
        "a": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
        "b": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
    },
    {
        "_id": {
            "_id": "second",
            "a": 1,
            "b": 2,
        },
        "a": {
            "_id": "first",
            "a": 1,
            "b": 1,
        },
        "b": {
            "_id": "second",
            "a": 1,
            "b": 2,
        },
    },
];

assertArrayEq({actual, expected});
