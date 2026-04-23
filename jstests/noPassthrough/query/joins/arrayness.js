/**
 * End to end test for join optimization being enabled iff no join predicate fields may contain arrays.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */

import {runTestWithUnorderedComparison, joinTestWrapper} from "jstests/libs/query/join_utils.js";

// Must enable path arrayness tracking for this test.
const conn = MongoRunner.runMongod({setParameter: "featureFlagPathArrayness=true"});
const db = conn.getDB(`${jsTestName()}_db`);

joinTestWrapper(db, function runArraynessTest() {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true, internalEnablePathArrayness: true}),
    );

    const c1 = db.c1;
    const c2 = db.c2;
    const c3 = db.c3;

    c1.drop();
    c2.drop();
    c3.drop();

    assert.commandWorked(
        c1.insertMany([
            {
                _id: 0,
                alwaysArray: [],
                sometimesArray: 3,
                neverArray: 1,
                obj: {array: [1, 2, 3], scalar: 1},
            },
            {
                _id: 1,
                alwaysArray: [1, 2, 3],
                sometimesArray: 2,
                neverArray: 1,
                obj: {},
            },
            {
                _id: 2,
                alwaysArray: [2, 3],
                sometimesArray: [3, 4],
                neverArray: 1,
                obj: {array: [], scalar: 2},
            },
        ]),
    );

    assert.commandWorked(
        c2.insertMany([
            {_id: 0, a: 1},
            {_id: 1, a: 2},
            {_id: 2, a: 3},
        ]),
    );

    assert.commandWorked(
        c3.insertMany([
            {_id: 0, a: 1, obj: {array: [1, 2, 3], scalar: 1}},
            {_id: 1, a: 2, obj: {array: [], scalar: 2}},
            {_id: 2, a: 3, obj: {}},
        ]),
    );

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness (no indexes) => no joinopt (2 node, no suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "neverArray",
                    foreignField: "a",
                    as: "x",
                },
            },
            {$unwind: "$x"},
        ],
        expectedResults: [
            {
                "_id": 0,
                "alwaysArray": [],
                "sometimesArray": 3,
                "neverArray": 1,
                "x": {"_id": 0, "a": 1},
                obj: {array: [1, 2, 3], scalar: 1},
            },
            {
                "_id": 1,
                "alwaysArray": [1, 2, 3],
                "sometimesArray": 2,
                "neverArray": 1,
                "x": {"_id": 0, "a": 1},
                obj: {},
            },
            {
                "_id": 2,
                "alwaysArray": [2, 3],
                "sometimesArray": [3, 4],
                "neverArray": 1,
                "x": {"_id": 0, "a": 1},
                obj: {array: [], scalar: 2},
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness => no joinopt ($expr, 2 node, no suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    as: "x",
                    let: {neverArray: "$neverArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$a", "$$neverArray"]}}}],
                },
            },
            {$unwind: "$x"},
        ],
        expectedResults: [
            {
                "_id": 0,
                "alwaysArray": [],
                "sometimesArray": 3,
                "neverArray": 1,
                "obj": {
                    "array": [1, 2, 3],
                    "scalar": 1,
                },
                "x": {
                    "_id": 0,
                    "a": 1,
                },
            },
            {
                "_id": 1,
                "alwaysArray": [1, 2, 3],
                "sometimesArray": 2,
                "neverArray": 1,
                "obj": {},
                "x": {
                    "_id": 0,
                    "a": 1,
                },
            },
            {
                "_id": 2,
                "alwaysArray": [2, 3],
                "sometimesArray": [3, 4],
                "neverArray": 1,
                "obj": {
                    "array": [],
                    "scalar": 2,
                },
                "x": {
                    "_id": 0,
                    "a": 1,
                },
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness => no joinopt (2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "neverArray",
                    foreignField: "a",
                    as: "x",
                },
            },
            {$unwind: "$x"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "x": {"_id": 0, "a": 1}},
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness => no joinopt ($expr, 2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    as: "x",
                    let: {neverArray: "$neverArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$$neverArray", "$a"]}}}],
                },
            },
            {$unwind: "$x"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "x": {"_id": 0, "a": 1}},
        ],
        expectedUsedJoinOptimization: false,
    });

    assert.commandWorked(c1.createIndex({neverArray: 1}));

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness on foreign field => no joinopt (2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "neverArray",
                    foreignField: "a",
                    as: "x",
                },
            },
            {$unwind: "$x"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "x": {"_id": 0, "a": 1}},
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness on foreign field => no joinopt ($expr, 2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    as: "x",
                    let: {neverArray: "$neverArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$$neverArray", "$a"]}}}],
                },
            },
            {$unwind: "$x"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "x": {"_id": 0, "a": 1}},
        ],
        expectedUsedJoinOptimization: false,
    });

    assert.commandWorked(c2.createIndex({a: 1}));

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness on local field => no joinopt (2 node, no suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "sometimesArray",
                    foreignField: "a",
                    as: "x",
                },
            },
            {$unwind: "$x"},
        ],
        expectedResults: [
            {
                "_id": 0,
                "alwaysArray": [],
                "sometimesArray": 3,
                "neverArray": 1,
                "obj": {
                    "array": [1, 2, 3],
                    "scalar": 1,
                },
                "x": {
                    "_id": 2,
                    "a": 3,
                },
            },
            {
                "_id": 1,
                "alwaysArray": [1, 2, 3],
                "sometimesArray": 2,
                "neverArray": 1,
                "obj": {},
                "x": {
                    "_id": 1,
                    "a": 2,
                },
            },
            {
                "_id": 2,
                "alwaysArray": [2, 3],
                "sometimesArray": [3, 4],
                "neverArray": 1,
                "obj": {
                    "array": [],
                    "scalar": 2,
                },
                "x": {
                    "_id": 2,
                    "a": 3,
                },
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "No arrayness on local field => no joinopt ($expr, 2 node, no suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    as: "x",
                    let: {sometimesArray: "$sometimesArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$$sometimesArray", "$a"]}}}],
                },
            },
            {$unwind: "$x"},
        ],
        expectedResults: [
            {
                "_id": 0,
                "alwaysArray": [],
                "sometimesArray": 3,
                "neverArray": 1,
                "obj": {
                    "array": [1, 2, 3],
                    "scalar": 1,
                },
                "x": {
                    "_id": 2,
                    "a": 3,
                },
            },
            {
                "_id": 1,
                "alwaysArray": [1, 2, 3],
                "sometimesArray": 2,
                "neverArray": 1,
                "obj": {},
                "x": {
                    "_id": 1,
                    "a": 2,
                },
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields => join opt (2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "neverArray",
                    foreignField: "a",
                    as: "x",
                },
            },
            {$unwind: "$x"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "x": {"_id": 0, "a": 1}},
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 1,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields => join opt ($expr, 2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    as: "x",
                    let: {na: "$neverArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$$na", "$a"]}}}],
                },
            },
            {$unwind: "$x"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "x": {"_id": 0, "a": 1}},
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 1,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields => join opt (2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "neverArray",
                    foreignField: "a",
                    as: "sometimesArray", // Arrayness of "as" field doesn't matter.
                },
            },
            {$unwind: "$sometimesArray"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": {"_id": 0, "a": 1}, "neverArray": 1},
            {"alwaysArray": [1, 2, 3], "sometimesArray": {"_id": 0, "a": 1}, "neverArray": 1},
            {"alwaysArray": [2, 3], "sometimesArray": {"_id": 0, "a": 1}, "neverArray": 1},
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 1,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields => join opt ($expr, 2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    as: "sometimesArray", // Arrayness of "as" field doesn't matter.
                    let: {na: "$neverArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$$na", "$a"]}}}],
                },
            },
            {$unwind: "$sometimesArray"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": {"_id": 0, "a": 1}, "neverArray": 1},
            {"alwaysArray": [1, 2, 3], "sometimesArray": {"_id": 0, "a": 1}, "neverArray": 1},
            {"alwaysArray": [2, 3], "sometimesArray": {"_id": 0, "a": 1}, "neverArray": 1},
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 1,
    });

    assert.commandWorked(c1.createIndexes([{sometimesArray: -1}, {alwaysArray: 1}]));

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields, multikey localField => no join opt (2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "sometimesArray",
                    foreignField: "a",
                    as: "y",
                },
            },
            {$unwind: "$y"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "y": {"_id": 2, "a": 3}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "y": {"_id": 1, "a": 2}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "y": {"_id": 2, "a": 3}},
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields, multikey localField => no join opt ($expr, 2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    as: "y",
                    let: {sa: "$sometimesArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$a", "$$sa"]}}}],
                },
            },
            {$unwind: "$y"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "y": {"_id": 2, "a": 3}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "y": {"_id": 1, "a": 2}},
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields, multikey foreignField => no join opt (2 node, suffix)",
        coll: c2,
        pipeline: [
            {
                $lookup: {
                    from: c1.getName(),
                    localField: "a",
                    foreignField: "alwaysArray",
                    as: "y",
                },
            },
            {$unwind: "$y"},
            {$project: {_id: 0, "y.obj": 0}},
        ],
        expectedResults: [
            {"a": 1, "y": {"_id": 1, "alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1}},
            {"a": 2, "y": {"_id": 1, "alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1}},
            {"a": 2, "y": {"_id": 2, "alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1}},
            {"a": 3, "y": {"_id": 1, "alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1}},
            {"a": 3, "y": {"_id": 2, "alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1}},
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields, multikey foreignField => no join opt ($expr, 2 node, suffix)",
        coll: c2,
        pipeline: [
            {
                $lookup: {
                    from: c1.getName(),
                    as: "y",
                    let: {aaa: "$a"},
                    pipeline: [{$match: {$expr: {$eq: ["$alwaysArray", "$$aaa"]}}}],
                },
            },
            {$unwind: "$y"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields, multikey foreignField/localField => no join opt (2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c1.getName(),
                    localField: "sometimesArray",
                    foreignField: "alwaysArray",
                    as: "y",
                },
            },
            {$unwind: "$y"},
            {$project: {_id: 0, obj: 0, "y.obj": 0}},
        ],
        expectedResults: [
            {
                "alwaysArray": [],
                "sometimesArray": 3,
                "neverArray": 1,
                "y": {"_id": 1, "alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1},
            },
            {
                "alwaysArray": [],
                "sometimesArray": 3,
                "neverArray": 1,
                "y": {"_id": 2, "alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1},
            },
            {
                "alwaysArray": [1, 2, 3],
                "sometimesArray": 2,
                "neverArray": 1,
                "y": {"_id": 1, "alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1},
            },
            {
                "alwaysArray": [1, 2, 3],
                "sometimesArray": 2,
                "neverArray": 1,
                "y": {"_id": 2, "alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1},
            },
            {
                "alwaysArray": [2, 3],
                "sometimesArray": [3, 4],
                "neverArray": 1,
                "y": {"_id": 1, "alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1},
            },
            {
                "alwaysArray": [2, 3],
                "sometimesArray": [3, 4],
                "neverArray": 1,
                "y": {"_id": 2, "alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1},
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    runTestWithUnorderedComparison({
        db,
        description: "Arrayness on all fields, multikey foreignField/localField => no join opt ($expr, 2 node, suffix)",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c1.getName(),
                    as: "y",
                    let: {sa: "$sometimesArray"},
                    pipeline: [{$match: {$expr: {$eq: ["$alwaysArray", "$$sa"]}}}],
                },
            },
            {$unwind: "$y"},
        ],
        expectedResults: [],
        expectedUsedJoinOptimization: false,
    });

    // Ensure we have arrayness info for c3 & obj field in c1.
    assert.commandWorked(c3.createIndex({a: -1, obj: 1}));
    assert.commandWorked(c1.createIndex({obj: 1}));

    runTestWithUnorderedComparison({
        db,
        description: "As field has an array subfield, used in subsequent join => no join opt in suffix",
        coll: c2,
        pipeline: [
            // This is ok, should use join opt.
            {
                $lookup: {
                    from: c1.getName(),
                    localField: "a",
                    foreignField: "neverArray",
                    as: "y",
                },
            },
            {$unwind: "$y"},
            // Prefix should end here: next predicate involves an array.
            {
                $lookup: {
                    from: c3.getName(),
                    localField: "y.sometimesArray",
                    foreignField: "a",
                    as: "z",
                },
            },
            {$unwind: "$z"},
            {$project: {"y.obj": 0, "z.obj": 0}},
        ],
        expectedResults: [
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 0,
                    "alwaysArray": [],
                    "sometimesArray": 3,
                    "neverArray": 1,
                },
                "z": {
                    "_id": 2,
                    "a": 3,
                },
            },
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 1,
                    "alwaysArray": [1, 2, 3],
                    "sometimesArray": 2,
                    "neverArray": 1,
                },
                "z": {
                    "_id": 1,
                    "a": 2,
                },
            },
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 2,
                    "alwaysArray": [2, 3],
                    "sometimesArray": [3, 4],
                    "neverArray": 1,
                },
                "z": {
                    "_id": 2,
                    "a": 3,
                },
            },
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 1, // We should not see a second!
    });

    runTestWithUnorderedComparison({
        db,
        description: "As field has scalar subfield, used in subsequent join => no join opt in suffix",
        coll: c2,
        pipeline: [
            // This is ok, should use join opt.
            {
                $lookup: {
                    from: c1.getName(),
                    as: "y",
                    let: {aaa: "$a"},
                    pipeline: [{$match: {$expr: {$eq: ["$$aaa", "$neverArray"]}}}],
                },
            },
            {$unwind: "$y"},
            // The following is as well! We should have the whole pipeline in our eligible prefix.
            {
                $lookup: {
                    from: c3.getName(),
                    localField: "y.neverArray",
                    foreignField: "a",
                    as: "z",
                },
            },
            {$unwind: "$z"},
            {$project: {"y.obj": 0, "z.obj": 0}},
        ],
        expectedResults: [
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 0,
                    "alwaysArray": [],
                    "sometimesArray": 3,
                    "neverArray": 1,
                },
                "z": {
                    "_id": 0,
                    "a": 1,
                },
            },
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 1,
                    "alwaysArray": [1, 2, 3],
                    "sometimesArray": 2,
                    "neverArray": 1,
                },
                "z": {
                    "_id": 0,
                    "a": 1,
                },
            },
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 2,
                    "alwaysArray": [2, 3],
                    "sometimesArray": [3, 4],
                    "neverArray": 1,
                },
                "z": {
                    "_id": 0,
                    "a": 1,
                },
            },
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 2, // Both $lookups should be pushed down!
    });

    runTestWithUnorderedComparison({
        db,
        description: "Test arrayness check works for subfields + compound join predicates.",
        coll: c2,
        pipeline: [
            // This is ok, should use join opt.
            {
                $lookup: {
                    from: c1.getName(),
                    as: "y",
                    let: {aaa: "$a"},
                    pipeline: [
                        {
                            $match: {
                                $expr: {
                                    $and: [
                                        {$eq: ["$$aaa", "$neverArray"]},
                                        {$gt: ["$sometimesArray", 0]}, // Residual predicate, should still be ok.
                                    ],
                                },
                            },
                        },
                    ],
                },
            },
            {$unwind: "$y"},
            // The following should be ok as well.
            {
                $lookup: {
                    from: c3.getName(),
                    as: "z",
                    let: {ooo: "$y.obj"},
                    pipeline: [{$match: {$expr: {$eq: ["$obj", "$$ooo"]}}}],
                },
            },
            {$unwind: "$z"},
            // But not this (since we don't have arrayness for obj.scalar).
            {
                $lookup: {
                    from: c1.getName(),
                    as: "w",
                    let: {ooo: "$z.obj.scalar"},
                    pipeline: [{$match: {$expr: {$eq: ["$neverArray", "$$ooo"]}}}],
                },
            },
            {$unwind: "$w"},
        ],
        expectedResults: [
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 0,
                    "alwaysArray": [],
                    "sometimesArray": 3,
                    "neverArray": 1,
                    "obj": {
                        "array": [1, 2, 3],
                        "scalar": 1,
                    },
                },
                "z": {
                    "_id": 0,
                    "a": 1,
                    "obj": {
                        "array": [1, 2, 3],
                        "scalar": 1,
                    },
                },
                "w": {
                    "_id": 0,
                    "alwaysArray": [],
                    "sometimesArray": 3,
                    "neverArray": 1,
                    "obj": {
                        "array": [1, 2, 3],
                        "scalar": 1,
                    },
                },
            },
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 0,
                    "alwaysArray": [],
                    "sometimesArray": 3,
                    "neverArray": 1,
                    "obj": {
                        "array": [1, 2, 3],
                        "scalar": 1,
                    },
                },
                "z": {
                    "_id": 0,
                    "a": 1,
                    "obj": {
                        "array": [1, 2, 3],
                        "scalar": 1,
                    },
                },
                "w": {
                    "_id": 1,
                    "alwaysArray": [1, 2, 3],
                    "sometimesArray": 2,
                    "neverArray": 1,
                    "obj": {},
                },
            },
            {
                "_id": 0,
                "a": 1,
                "y": {
                    "_id": 0,
                    "alwaysArray": [],
                    "sometimesArray": 3,
                    "neverArray": 1,
                    "obj": {
                        "array": [1, 2, 3],
                        "scalar": 1,
                    },
                },
                "z": {
                    "_id": 0,
                    "a": 1,
                    "obj": {
                        "array": [1, 2, 3],
                        "scalar": 1,
                    },
                },
                "w": {
                    "_id": 2,
                    "alwaysArray": [2, 3],
                    "sometimesArray": [3, 4],
                    "neverArray": 1,
                    "obj": {
                        "array": [],
                        "scalar": 2,
                    },
                },
            },
        ],
        expectedUsedJoinOptimization: true,
        expectedNumJoinStages: 2,
    });

    // Disabling internalEnablePathArrayness should prevent join optimization from using arrayness
    // info, so a query that previously qualified for joinopt must no longer qualify.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnablePathArrayness: false}));

    runTestWithUnorderedComparison({
        db,
        description: "internalEnablePathArrayness=false => no joinopt even when arrayness is known",
        coll: c1,
        pipeline: [
            {
                $lookup: {
                    from: c2.getName(),
                    localField: "neverArray",
                    foreignField: "a",
                    as: "x",
                },
            },
            {$unwind: "$x"},
            {$project: {_id: 0, obj: 0}},
        ],
        expectedResults: [
            {"alwaysArray": [], "sometimesArray": 3, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [1, 2, 3], "sometimesArray": 2, "neverArray": 1, "x": {"_id": 0, "a": 1}},
            {"alwaysArray": [2, 3], "sometimesArray": [3, 4], "neverArray": 1, "x": {"_id": 0, "a": 1}},
        ],
        expectedUsedJoinOptimization: false,
    });
});

MongoRunner.stopMongod(conn);
