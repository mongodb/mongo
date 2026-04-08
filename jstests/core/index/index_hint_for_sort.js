/**
 * Verify that queries with index hint return correct results with enabled sort optimization.
 * @tags: [
 *   # Retrieving results using toArray may require a getMore command.
 *   requires_getmore,
 *   # Test creates specific indexes and assumes no implicit indexes are added.
 *   assumes_no_implicit_index_creation,
 * ]
 */

const testCases = [
    {
        data: [{"_id": 122, "a": 0}],
        indexes: [],
        pipeline: [
            {
                "$match": {
                    "a": {
                        "$elemMatch": {
                            "$nin": [],
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
        hint: "_id_",
        comment: "Test case: ElemMatchValue without index.",
    },
    {
        data: [{"_id": 122, "a": 0}],
        indexes: [],
        pipeline: [
            {
                "$match": {
                    "a": {
                        "$elemMatch": {
                            "$nin": [],
                        },
                    },
                },
            },
        ],
        hint: "_id_",
        comment: "Test case: ElemMatchValue without projection.",
    },
    {
        data: [{"_id": 122, "a": 0}],
        indexes: [{a: 1}],
        pipeline: [
            {
                "$match": {
                    "a": {
                        "$elemMatch": {
                            "$nin": [],
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
        hint: "_id_",
        comment: "Test case: ElemMatchValue with index.",
    },
    {
        data: [{"_id": 122, "a": {b: 100, c: 200}}],
        indexes: [],
        pipeline: [
            {
                "$match": {
                    "a": {
                        "$elemMatch": {
                            b: 100,
                            c: 200,
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
        hint: "_id_",
        comment: "ElemMatchObject without index.",
    },
    {
        data: [{"_id": 122, "a": {b: 100, c: 200}}],
        indexes: [{"a.b": 1}, {"a.c": 1}],
        pipeline: [
            {
                "$match": {
                    "a": {
                        "$elemMatch": {
                            b: 100,
                            c: 200,
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
        hint: "_id_",
        comment: "ElemMatchObject with indexes.",
    },
    {
        data: [{"_id": 122, "a": [1]}],
        indexes: [],
        pipeline: [
            {
                "$match": {
                    "a": {$size: 1},
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "a": 1,
                },
            },
        ],
        hint: "_id_",
        comment: "$size without index.",
    },
    {
        data: [{"_id": 122, "a": [1]}],
        indexes: [{a: 1}],
        pipeline: [
            {
                "$match": {
                    "a": {$size: 2},
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "a": 1,
                },
            },
        ],
        hint: "_id_",
        comment: "$size with index.",
    },
];

function runTestCase(testCase) {
    const coll = db[jsTestName()];
    coll.drop();

    assert.commandWorked(coll.insertMany(testCase.data));
    for (const index of testCase.indexes) {
        assert.commandWorked(coll.createIndex(index));
    }

    const expectedResult = coll.aggregate(testCase.pipeline).toArray();
    const hintedResult = coll.aggregate(testCase.pipeline, {hint: testCase.hint}).toArray();

    assert.eq(expectedResult, hintedResult, testCase.comment);
}

for (const testCase of testCases) {
    runTestCase(testCase);
}
