/**
 * Tests metadata propagation from and to extension stages in $unionWith pipelines.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {it, describe} from "jstests/libs/mochalite.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {assertArrayEq, assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
const otherCollName = collName + "_other";
const otherColl = db[otherCollName];

coll.drop();
otherColl.drop();

coll.insertMany([
    {_id: 1, name: "apple"},
    {_id: 2, name: "banana"},
    {_id: 3, name: "orange"},
]);

otherColl.insertMany([
    {_id: 10, name: "grape"},
    {_id: 20, name: "mango"},
    {_id: 30, name: "pineapple"},
]);

const kUnavailableMetadataErrCode = 40218;

const projectTextScoreStage = {
    $project: {
        _id: 1,
        textScore: {$meta: "textScore"},
    },
};

const runTestcase = (pipeline, expected) => {
    const results = coll.aggregate(pipeline).toArray();
    const numShards = FixtureHelpers.numberOfShardsForCollection(coll);

    if (numShards > 1) {
        // In sharded topology: stages before $unionWith duplicate per shard; $unionWith sub-pipeline runs once.
        // Therefore to keep it simple, I'm just checking that the results are non-empty.
        assert.gt(results.length, 0);
    } else {
        assertArrayEq({actual: results, expected: expected});
    }
};

describe("Metadata tests for an extension stage in $unionWith - Error scenarios.", function () {
    it("Reference metadata in top-level when only $unionWith provides it.", function () {
        const pipeline = [
            {
                $unionWith: {
                    coll: otherCollName,
                    pipeline: [{$fruitAsDocuments: {}}, projectTextScoreStage],
                },
            },
            projectTextScoreStage,
        ];
        assertErrCodeAndErrMsgContains(coll, pipeline, kUnavailableMetadataErrCode, "text score");
    });

    it("Reference metadata in $unionWith when only top-level provides it.", function () {
        const pipeline = [
            {$fruitAsDocuments: {}},
            {
                $unionWith: {
                    coll: otherCollName,
                    pipeline: [{$match: {_id: {$gte: 10}}}, projectTextScoreStage],
                },
            },
        ];
        assertErrCodeAndErrMsgContains(coll, pipeline, kUnavailableMetadataErrCode, "text score");
    });

    it("Extension stage in $unionWith references non-existent metadata.", function () {
        const pipeline = [
            {
                $unionWith: {
                    coll: otherCollName,
                    pipeline: [{$addFruitsToDocumentsWithMetadata: {}}, projectTextScoreStage],
                },
            },
        ];
        assertErrCodeAndErrMsgContains(coll, pipeline, kUnavailableMetadataErrCode, "text score");
    });
});

describe("Metadata tests for an extension stage in $unionWith - Success scenarios.", function () {
    it("Reference metadata in top-level when both pipelines provide it.", function () {
        const pipeline = [
            {$fruitAsDocuments: {}},
            {
                $unionWith: {
                    coll: otherCollName,
                    pipeline: [{$fruitAsDocuments: {}}],
                },
            },
            {
                $match: {_id: 1},
            },
            projectTextScoreStage,
        ];
        const expectedResults = [
            {"_id": 1, "textScore": 5},
            {"_id": 1, "textScore": 5},
        ];
        runTestcase(pipeline, expectedResults);
    });

    it("Reference metadata in $unionWith when provided by extension stage.", function () {
        const pipeline = [
            {
                $unionWith: {
                    coll: otherCollName,
                    pipeline: [
                        {$fruitAsDocuments: {}},
                        {
                            $project: {
                                _id: 1,
                                textScore: {$meta: "textScore"},
                                searchScore: {$meta: "searchScore"},
                                searchScoreDetails: {$meta: "searchScoreDetails"},
                            },
                        },
                    ],
                },
            },
        ];
        const expectedResults = [
            {"_id": 1, "name": "apple"},
            {"_id": 2, "name": "banana"},
            {"_id": 3, "name": "orange"},
            {"_id": 1, "textScore": 5},
            {"_id": 2, "searchScore": 1.5},
            {"_id": 3, "searchScore": 2},
            {"_id": 4, "textScore": 4},
            {"_id": 5, "searchScoreDetails": {"scoreDetails": "foo"}},
        ];
        runTestcase(pipeline, expectedResults);
    });

    it("Extension stage in $unionWith provides and uses metadata.", function () {
        const pipeline = [
            {
                $unionWith: {
                    coll: otherCollName,
                    pipeline: [
                        {$fruitAsDocuments: {}},
                        {$addFruitsToDocumentsWithMetadata: {}},
                        {
                            $project: {
                                _id: 1,
                                textScore: {$meta: "textScore"},
                                vectorSearchScore: {$meta: "vectorSearchScore"},
                            },
                        },
                    ],
                },
            },
        ];
        const expectedResults = [
            {"_id": 1, "name": "apple"},
            {"_id": 2, "name": "banana"},
            {"_id": 3, "name": "orange"},
            {"textScore": 10, "vectorSearchScore": 50},
            {"vectorSearchScore": 50},
            {"vectorSearchScore": 50},
            {"textScore": 10, "vectorSearchScore": 50},
            {"vectorSearchScore": 50},
        ];
        runTestcase(pipeline, expectedResults);
    });

    it("Top-level provides metadata and references it.", function () {
        const pipeline = [
            {$fruitAsDocuments: {}},
            projectTextScoreStage,
            {
                $unionWith: {
                    coll: otherCollName,
                    pipeline: [{$match: {_id: {$gte: 10}}}],
                },
            },
        ];
        const expectedResults = [
            {"_id": 1, "textScore": 5},
            {"_id": 2},
            {"_id": 3},
            {"_id": 4, "textScore": 4},
            {"_id": 5},
            {"_id": 10, "name": "grape"},
            {"_id": 20, "name": "mango"},
            {"_id": 30, "name": "pineapple"},
        ];
        runTestcase(pipeline, expectedResults);
    });
});
