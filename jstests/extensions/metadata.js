/**
 * Tests metadata propagation from and to extension.
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

coll.insertMany([
    {_id: 1, name: "apple"},
    {_id: 2, name: "banana"},
    {_id: 3, name: "orange"},
    {_id: 4, name: "watermelon"},
    {_id: 5, name: "kiwi"},
]);

const runTestcase = (name, pipeline, expected, hasDupResults) => {
    const results = coll.aggregate(pipeline).toArray();
    if (FixtureHelpers.numberOfShardsForCollection(coll) > 1 && hasDupResults) {
        // TODO SERVER-114234.
        assert.gt(results.length, 0, results);
    } else {
        assertArrayEq({actual: results, expected: expected, extraErrorMsg: name});
    }
};

const tests = [
    {
        name: "1.Metadata on a source stage.",
        pipeline: [
            {$fruitAsDocuments: {}},
            {
                $project: {
                    _id: 1,
                    textScore: {$meta: "textScore"},
                    searchScore: {$meta: "searchScore"},
                    scoreDetails: {$meta: "searchScoreDetails"},
                },
            },
        ],
        expected: [
            {"_id": 1, "textScore": 5.0},
            {"_id": 2, "searchScore": 1.5},
            {"_id": 3, "searchScore": 2.0},
            {"_id": 4, "textScore": 4.0},
            {"_id": 5, "scoreDetails": {"scoreDetails": "foo"}},
        ],
        hasDupResults: true,
    },
    {
        name: "2.Metadata gets passed through an extension transform stage.",
        pipeline: [
            {$fruitAsDocuments: {}},
            {$addFruitsToDocuments: {}},
            {
                $project: {
                    textScore: {$meta: "textScore"},
                    searchScore: {$meta: "searchScore"},
                    scoreDetails: {$meta: "searchScoreDetails"},
                },
            },
        ],
        expected: [
            {"textScore": 5.0},
            {"searchScore": 1.5},
            {"searchScore": 2.0},
            {"textScore": 4.0},
            {"scoreDetails": {"scoreDetails": "foo"}},
        ],
        hasDupResults: true,
    },
    {
        name: "3.Transform extension adds metadata.",
        pipeline: [
            {$fruitAsDocuments: {}},
            {$addFruitsToDocumentsWithMetadata: {}},
            {
                $project: {
                    textScore: {$meta: "textScore"},
                    searchScore: {$meta: "searchScore"},
                    vectorSearchScore: {$meta: "vectorSearchScore"},
                },
            },
        ],
        expected: [
            {"textScore": 10.0, "vectorSearchScore": 50.0},
            {"searchScore": 10.0, "vectorSearchScore": 50.0},
            {"searchScore": 10.0, "vectorSearchScore": 50.0},
            {"textScore": 10.0, "vectorSearchScore": 50.0},
            {"vectorSearchScore": 50.0},
        ],
        hasDupResults: true,
    },
    {
        name: "4.Non-extension stage before a transform extension stage that adds metadata.",
        pipeline: [
            {$match: {name: {$in: ["apple", "banana", "orange"]}}},
            {$addFruitsToDocumentsWithMetadata: {}},
            {
                $project: {
                    existingDoc: 1,
                    vectorSearchScore: {$meta: "vectorSearchScore"},
                },
            },
        ],
        expected: [
            {
                "existingDoc": {
                    "_id": 1,
                    "name": "apple",
                },
                "vectorSearchScore": 50.0,
            },
            {
                "existingDoc": {
                    "_id": 2,
                    "name": "banana",
                },
                "vectorSearchScore": 50.0,
            },
            {
                "existingDoc": {
                    "_id": 3,
                    "name": "orange",
                },
                "vectorSearchScore": 50.0,
            },
        ],
        hasDupResults: false,
    },
    {
        name: "5.Metadata is preserved across multiple pipeline stages.",
        pipeline: [
            {$fruitAsDocuments: {}},
            {$match: {_id: {$in: [1, 2]}}},
            {$addFruitsToDocumentsWithMetadata: {}},
            {
                $project: {
                    existingDoc: 1,
                    textScore: {$meta: "textScore"},
                    searchScore: {$meta: "searchScore"},
                    vectorSearchScore: {$meta: "vectorSearchScore"},
                },
            },
        ],
        expected: [
            {
                "existingDoc": {
                    "_id": 1,
                    "apples": "red",
                },
                "textScore": 10.0,
                "vectorSearchScore": 50.0,
            },
            {
                "existingDoc": {
                    "_id": 2,
                    "oranges": 5,
                },
                "searchScore": 10.0,
                "vectorSearchScore": 50.0,
            },
        ],
        hasDupResults: true,
    },
];

// Run all tests.
tests.forEach((test) => {
    runTestcase(test.name, test.pipeline, test.expected, test.hasDupResults);
});

{
    const name = "6.Support $sortKey metadata.";
    const pipeline = [
        {$validateMultiKeySort: {}},
        {
            $project: {
                _id: 1,
                textScore: {$meta: "textScore"},
                searchScore: {$meta: "searchScore"},
            },
        },
    ];
    const expected = [
        {"_id": 1},
        {
            "_id": 2,
            "textScore": 4.0,
            "searchScore": 0.0,
        },
        {
            "_id": 3,
            "textScore": 5.0,
            "searchScore": 0.0,
        },
        {
            "_id": 4,
            "textScore": 0.0,
            "searchScore": 1.5,
        },
        {
            "_id": 5,
            "textScore": 0.0,
            "searchScore": 2.0,
        },
    ];
    // Ordering is deterministic so using assert.eq.
    const results = coll.aggregate(pipeline).toArray();
    assert.eq(results, expected, name);
}

{
    const name = "7.Extension that doesn't preserve upstream metadata.";
    const pipeline = [
        {$fruitAsDocuments: {}},
        {
            $addFruitsToDocumentsWithMetadata: {preserveUpstreamMetadata: false},
        },
        {
            $project: {
                _id: 1,
                score: {$meta: "scoreDetails"},
            },
        },
    ];

    // In sharded collection, it throws an error as we do meta dependency analysis which is required by the planner to split the pipeline.
    // In single shard, meta dependency analysis isn't executed. however, "score" field does not appear since there is no "scoreDetails" metadata.
    if (FixtureHelpers.numberOfShardsForCollection(coll) > 1) {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: collName,
                pipeline: pipeline,
                cursor: {},
            }),
            40218,
        );
    } else {
        runTestcase(name, pipeline, [{}, {}, {}, {}, {score: {"scoreDetails": "foo"}}], false);
    }
}
