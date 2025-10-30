/**
 * Ensures that $elemMatch dependency analysis works correctly when attempting to apply a projection pushdown to the find layer.
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
assert(coll.drop());
assert.commandWorked(coll.insert([{}]));

{
    // Repro case from SERVER-112844.
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {
                    "$addFields": {
                        "a": 0,
                    },
                },
                {
                    "$match": {
                        "a": {
                            "$elemMatch": {
                                "$elemMatch": {
                                    "$eq": 0,
                                },
                                "$in": [0],
                            },
                        },
                    },
                },
                {
                    "$project": {
                        "_id": 0,
                        "a": 1,
                    },
                },
            ],
            cursor: {},
        }),
    );
    assert.eq(res.cursor.firstBatch.length, 0);
}

{
    // Reverse predicates in $elemMatch.
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {
                    "$addFields": {
                        "a": 0,
                    },
                },
                {
                    "$match": {
                        "a": {
                            "$elemMatch": {
                                "$in": [0],
                                "$elemMatch": {
                                    "$eq": 0,
                                },
                            },
                        },
                    },
                },
                {
                    "$project": {
                        "_id": 0,
                        "a": 1,
                    },
                },
            ],
            cursor: {},
        }),
    );
    assert.eq(res.cursor.firstBatch.length, 0);
}

{
    // Repeat for a version which should return a result.
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {
                    "$addFields": {
                        "a": [0],
                    },
                },
                {
                    "$match": {
                        "a": {
                            "$elemMatch": {
                                "$elemMatch": {
                                    "$eq": 0,
                                },
                                "$in": [0],
                            },
                        },
                    },
                },
                {
                    "$project": {
                        "_id": 0,
                        "a": 1,
                    },
                },
            ],
            cursor: {},
        }),
    );
    arrayEq(res.cursor.firstBatch, [{a: [0]}]);
}

{
    // Even greater $elemMatch nesting.
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {
                    "$addFields": {
                        "a": [[0]],
                    },
                },
                {
                    "$match": {
                        "a": {
                            "$elemMatch": {
                                "$elemMatch": {
                                    "$elemMatch": {
                                        "$eq": 0,
                                    },
                                },
                                "$in": [[0]],
                            },
                        },
                    },
                },
                {
                    "$project": {
                        "_id": 0,
                        "a": 1,
                    },
                },
            ],
            cursor: {},
        }),
    );
    arrayEq(res.cursor.firstBatch, [{a: [[0]]}]);
}

{
    // Even greater $elemMatch nesting.
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {
                    "$addFields": {
                        "a": [[[0]]],
                    },
                },
                {
                    "$match": {
                        "a": {
                            "$elemMatch": {
                                "$elemMatch": {
                                    "$elemMatch": {
                                        "$elemMatch": {
                                            "$eq": 0,
                                        },
                                    },
                                },
                                "$in": [[[0]]],
                            },
                        },
                    },
                },
                {
                    "$project": {
                        "_id": 0,
                        "a": 1,
                    },
                },
            ],
            cursor: {},
        }),
    );
    arrayEq(res.cursor.firstBatch, [{a: [[[0]]]}]);
}
