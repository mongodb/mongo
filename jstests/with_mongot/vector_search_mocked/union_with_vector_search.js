/**
 * Testing $unionWith: {$vectorSearch}
 */

import {getAggPlanStages} from "jstests/libs/analyze_plan.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = jsTestName();
const vectorSearchCollName = "vector_search";
const vectorSearchCollNS = dbName + "." + vectorSearchCollName;
const basicCollName = "basic";
const basicCollNS = dbName + "." + basicCollName;

// Start mock mongot.
const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

// Start mongod.
const conn = MongoRunner.runMongod({
    setParameter: {mongotHost: mockConn.host},
    verbose: 1,
});
const testDB = conn.getDB(dbName);
assertCreateCollection(testDB, vectorSearchCollName);
assertCreateCollection(testDB, basicCollName);

const vectorSearchColl = testDB.getCollection(vectorSearchCollName);
vectorSearchColl.insert({_id: 1, title: "Tarzan the Ape Man"});
vectorSearchColl.insert({_id: 2, title: "King Kong"});

const basicColl = testDB.getCollection(basicCollName);
basicColl.insert({_id: 100, localField: "cakes", weird: false});
basicColl.insert({_id: 101, localField: "cakes and kale", weird: true});

const vectorSearchCollectionUUID = getUUIDFromListCollections(testDB, vectorSearchCollName);
const basicCollectionUUID = getUUIDFromListCollections(testDB, basicCollName);

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const limit = 5;

const cursorId1 = NumberLong(123);
const cursorId2 = NumberLong(456);
const responseOk = 1;

const vsQuery = [
    {
        $vectorSearch: {
            queryVector,
            path,
            limit,
        }
    },
];

// Common use case of $vectorSearch + basic projection.
const vsPipeline = vsQuery.concat([
    {$project: {_id: 0, title: 1, score: {$meta: 'vectorSearchScore'}}},
]);

function setupVectorSearchResponse(cursorId = cursorId1) {
    const mongotResponseBatch =
        [{_id: 1, $vectorSearchScore: 1}, {_id: 2, $vectorSearchScore: 0.8}];
    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            limit,
            collName: vectorSearchCollName,
            dbName,
            collectionUUID: vectorSearchCollectionUUID
        }),
        response: mongotResponseForBatch(
            mongotResponseBatch, NumberLong(0), vectorSearchCollNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
}

(function testUnionWithVectorSearch() {
    // Basic collection $unionWith $vectorSearch.
    const basicUnionWith = [{
        $unionWith: {
            coll: vectorSearchCollName,
            pipeline: vsPipeline,
        }
    }];
    const basicUnionWithExpected = [
        {"_id": 100, "localField": "cakes", "weird": false},
        {"_id": 101, "localField": "cakes and kale", "weird": true},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse();
    const basicUnionWithResult = basicColl.aggregate(basicUnionWith).toArray();
    assert.eq(basicUnionWithResult, basicUnionWithExpected);

    // Check that the explain output of the above contains both $unionWith and $vectorSearch.
    // "queryPlanner" is currently the only supported verbosity option.
    const explainHistory = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            limit,
            collName: vectorSearchCollName,
            dbName,
            collectionUUID: vectorSearchCollectionUUID,
            explain: {verbosity: "queryPlanner"}
        }),
        response: {explain: {}, ok: 1}
    }];
    mongotMock.setMockResponses(explainHistory, cursorId1);

    const result = basicColl.explain("queryPlanner").aggregate(basicUnionWith);
    assert.neq(getAggPlanStages(result, "$unionWith"), null, result);
    assert.neq(getAggPlanStages(result, "$vectorSearch"), null, result);
})();

(function testVectorSearchUnionWithVectorSearch() {
    // $vectorSearch $unionWith $vectorSearch.
    const vsUnionWithVs1 = vsPipeline;
    const vsUnionWithVs2 = [
        {$project: {_id: 0, title: 1, score: {$meta: 'vectorSearchScore'}}},
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        }
    ];
    const vsUnionWithVs = vsUnionWithVs1.concat(vsUnionWithVs2);
    const vsUnionWithVsExpected = [
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse(cursorId1);
    setupVectorSearchResponse(cursorId2);

    const vsUnionWithVsResult = vectorSearchColl.aggregate(vsUnionWithVs).toArray();
    assert.eq(vsUnionWithVsResult, vsUnionWithVsExpected);
})();

(function testVectorSearchUnionWithDifferentCollVectorSearch() {
    // $vectorSearch on coll A $unionWith $vectorSearch on coll B.
    const vsUnionWithVsCollB = vsPipeline.concat([{
        $unionWith: {
            coll: basicCollName,
            pipeline: vsPipeline,
        }
    }]);
    const vsUnionWithVsCollBExpected = [
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
        {"score": 3},
        {"score": 2},
    ];

    const mongotResponseBatch2 =
        [{_id: 100, $vectorSearchScore: 3}, {_id: 101, $vectorSearchScore: 2}];
    const mongotRequestResponse2 = {
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            limit,
            collName: basicCollName,
            dbName,
            collectionUUID: basicCollectionUUID
        }),
        response:
            mongotResponseForBatch(mongotResponseBatch2, NumberLong(0), basicCollNS, responseOk),
    };

    setupVectorSearchResponse(cursorId1);
    mongotMock.setMockResponses([mongotRequestResponse2], cursorId2);

    const vsUnionWithVsCollBResult = vectorSearchColl.aggregate(vsUnionWithVsCollB).toArray();
    assert.eq(vsUnionWithVsCollBResult, vsUnionWithVsCollBExpected);
})();

(function testDualUnionWithVectorSearch() {
    // Multiple $unionWith $vectorSearch stages:  {$unionWith $vectorSearch}, {$unionWith
    // $vectorSearch}.
    const dualUnionWith = [
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        },
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        },
    ];
    const dualUnionWithExpected = [
        {"_id": 100, "localField": "cakes", "weird": false},
        {"_id": 101, "localField": "cakes and kale", "weird": true},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse(cursorId1);
    setupVectorSearchResponse(cursorId2);

    const dualUnionWithResult = basicColl.aggregate(dualUnionWith).toArray();
    assert.eq(dualUnionWithResult, dualUnionWithExpected);
})();

(function testNestedUnionWithVectorSearch() {
    const nestedUnionWithExpected = [
        {"_id": 100, "localField": "cakes", "weird": false},
        {"_id": 101, "localField": "cakes and kale", "weird": true},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
    ];

    // Nested $unionWith $vectorSearch (should get same results as above).
    const nestedUnionWith = [
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline.concat([
                    {
                        $unionWith: {
                            coll: vectorSearchCollName,
                            pipeline: vsPipeline,
                        }
                    },
                ]),
            }
        },
    ];

    setupVectorSearchResponse(cursorId1);
    setupVectorSearchResponse(cursorId2);

    const nestedUnionWithResult = basicColl.aggregate(nestedUnionWith).toArray();
    assert.eq(nestedUnionWithResult, nestedUnionWithExpected);
})();

(function testUnionWithVectorSearchUnionWith() {
    // $unionWith {$vectorSearch} after a non-$vectorSearch $unionWith.
    const twoDiffUnionWith = [
        {
            $unionWith: {
                coll: basicCollName,
            }
        },
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        },
    ];
    const twoDiffUnionWithExpected = [
        {"_id": 100, "localField": "cakes", "weird": false},
        {"_id": 101, "localField": "cakes and kale", "weird": true},
        {"_id": 100, "localField": "cakes", "weird": false},
        {"_id": 101, "localField": "cakes and kale", "weird": true},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse();

    const twoDiffUnionWithResult = basicColl.aggregate(twoDiffUnionWith).toArray();
    assert.eq(twoDiffUnionWithResult, twoDiffUnionWithExpected);
})();

(function testUnionWithUnionWithVectorSearch() {
    const twoDiffUnionWithFlipped = [
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        },
        {
            $unionWith: {
                coll: basicCollName,
            }
        },
    ];
    const twoDiffUnionWithFlippedExpected = [
        {"_id": 100, "localField": "cakes", "weird": false},
        {"_id": 101, "localField": "cakes and kale", "weird": true},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
        {"_id": 100, "localField": "cakes", "weird": false},
        {"_id": 101, "localField": "cakes and kale", "weird": true},
    ];

    setupVectorSearchResponse();

    const twoDiffUnionWithFlippedResult = basicColl.aggregate(twoDiffUnionWithFlipped).toArray();
    assert.eq(twoDiffUnionWithFlippedResult, twoDiffUnionWithFlippedExpected);
})();

(function testUnionWithVectorSearchMatch() {
    // Match stage after a $unionWith $vectorSearch.
    const matchAfterUnionWith = [
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        },
        {$match: {title: "King Kong"}},
    ];
    const matchAfterUnionWithExpected = [
        {"title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse();

    const matchAfterUnionWithResult = basicColl.aggregate(matchAfterUnionWith).toArray();
    assert.eq(matchAfterUnionWithResult, matchAfterUnionWithExpected);
})();

(function testAddFieldsBasicCollectionUnionWithVectorSearch() {
    // $addFields to basic collection (basicColl) prior to a $unionWith $vectorSearch.
    const addFieldsToBasicPreUnionWith = [
        {
            $addFields: {
                id2: "$_id",
            }
        },
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        },
    ];
    const addFieldsToBasicPreUnionWithExpected = [
        {"_id": 100, "localField": "cakes", "weird": false, "id2": 100},
        {"_id": 101, "localField": "cakes and kale", "weird": true, "id2": 101},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse();

    const addFieldsToBasicPreUnionWithResult =
        basicColl.aggregate(addFieldsToBasicPreUnionWith).toArray();
    assert.eq(addFieldsToBasicPreUnionWithResult, addFieldsToBasicPreUnionWithExpected);
})();

(function testAddFieldsVectorSearchCollUnionWithVectorSearch() {
    //$addFields to vector search collection (coll) prior to a $unionWith $vectorSearch.
    const addFieldsToVectorSearchCollPreUnionWith = [
        {
            $sort: {_id: 1},
        },
        {
            $limit: 1,
        },
        {
            $project: {title: 1},
        },
        {
            $addFields: {
                id2: "$_id",
            }
        },
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsPipeline,
            }
        },
    ];
    const addFieldsToVectorSearchCollPreUnionWithExpected = [
        {"_id": 1, "title": "Tarzan the Ape Man", "id2": 1},
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse();

    const addFieldsToVectorSearchCollPreUnionWithResult =
        vectorSearchColl.aggregate(addFieldsToVectorSearchCollPreUnionWith).toArray();
    assert.eq(addFieldsToVectorSearchCollPreUnionWithResult,
              addFieldsToVectorSearchCollPreUnionWithExpected);
})();

(function testUnionWithVectorSearchNestedAddFields() {
    // Adding (non-meta related) fields within $unionWith.
    const addFieldsWithinUnionWith = [
        {
            $sort: {_id: 1},
        },
        {
            $limit: 1,
        },
        {
            $project: {title: 1},
        },
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsQuery.concat([
                    {$project: {title: 1, score: {$meta: 'vectorSearchScore'}}},
                    {$addFields: {id2: "$_id"}},
                ]),
            }
        }
    ];
    const addFieldsWithinUnionWithExpected = [
        {"_id": 1, "title": "Tarzan the Ape Man"},
        {"_id": 1, "title": "Tarzan the Ape Man", "score": 1, "id2": 1},
        {"_id": 2, "title": "King Kong", "score": 0.8, "id2": 2},
    ];

    setupVectorSearchResponse();

    const addFieldsWithinUnionWithResult =
        vectorSearchColl.aggregate(addFieldsWithinUnionWith).toArray();
    assert.eq(addFieldsWithinUnionWithResult, addFieldsWithinUnionWithExpected);
})();

(function testUnionWithVectorSearchAddFields() {
    // Adding fields after $unionWith.
    const addFieldsAfterUnionWith = [
        {
            $sort: {_id: 1},
        },
        {
            $limit: 1,
        },
        {
            $project: {title: 1},
        },
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsQuery.concat([
                    {$project: {title: 1, score: {$meta: 'vectorSearchScore'}}},
                ]),
            }
        },
        {
            $addFields: {
                id2: "$_id",
            }
        },
    ];
    const addFieldsAfterUnionWithExpected = [
        {"_id": 1, "title": "Tarzan the Ape Man", "id2": 1},
        {"_id": 1, "title": "Tarzan the Ape Man", "score": 1, "id2": 1},
        {"_id": 2, "title": "King Kong", "score": 0.8, "id2": 2},
    ];

    setupVectorSearchResponse();

    const addFieldsAfterUnionWithResult =
        vectorSearchColl.aggregate(addFieldsAfterUnionWith).toArray();
    assert.eq(addFieldsAfterUnionWithResult, addFieldsAfterUnionWithExpected);
})();

(function testUnionWithVectorSearchSort() {
    // Sorting by score AFTER a unionWith.
    const sortAfterUnionWith = [
        {
            $addFields: {score: 2},
        },
        {
            $unionWith: {
                coll: vectorSearchCollName,
                pipeline: vsQuery.concat([
                    {$project: {_id: 1, title: 1, score: {$meta: 'vectorSearchScore'}}},
                ]),
            }
        },
        {$sort: {score: -1}},
    ];
    const sortAfterUnionWithExpected = [
        {"_id": 100, "localField": "cakes", "weird": false, "score": 2},
        {"_id": 101, "localField": "cakes and kale", "weird": true, "score": 2},
        {"_id": 1, "title": "Tarzan the Ape Man", "score": 1},
        {"_id": 2, "title": "King Kong", "score": 0.8},
    ];

    setupVectorSearchResponse();

    const sortAfterUnionWithResult = basicColl.aggregate(sortAfterUnionWith).toArray();
    assert.eq(sortAfterUnionWithResult, sortAfterUnionWithExpected);
})();

// Set up to test with a $lookup stage.
const lookupCollName = jsTestName() + "_lookup";
const lookupColl = testDB.getCollection(lookupCollName);

assert.commandWorked(lookupColl.insert({_id: 1, blob: true}));
assert.commandWorked(lookupColl.insert({_id: 2, blob: true}));

(function testLookupUnionWithVectorSearch() {
    // {$lookup (non-VS)}, {$unionWith $vectorSearch}.
    const lookupThenUnionWithVS = [
    {
        $lookup:
            { from: lookupColl.getName(), localField: "weird", foreignField: "blob", as: "result" }

    },
    {
        $unionWith: {
            coll: vectorSearchCollName,
            pipeline: vsPipeline,
        }
    },
];
    const lookupThenUnionWithVSExpected = [
        {"_id": 100, "localField": "cakes", "weird": false, "result": []},
        {
            "_id": 101,
            "localField": "cakes and kale",
            "weird": true,
            "result": [{"_id": 1, "blob": true}, {"_id": 2, "blob": true}]
        },
        {"title": "Tarzan the Ape Man", "score": 1},
        {"title": "King Kong", "score": 0.8}
    ];

    setupVectorSearchResponse();

    const lookupThenUnionWithVSResult = basicColl.aggregate(lookupThenUnionWithVS).toArray();
    assert.eq(lookupThenUnionWithVSResult, lookupThenUnionWithVSExpected);
})();

(function testUnionWithVectorSearchLookup() {
    // {$unionWith $vectorSearch}, {$lookup (non-VS)}.
    const unionWithVSThenLookup = [
    {
        $unionWith: {
            coll: vectorSearchCollName,
            pipeline: vsPipeline.concat([
                { $addFields: { "weird": { $lt: ["$score", 1] } } },
            ]),
        }
    },
    {
        $lookup:
            { from: lookupColl.getName(), localField: "weird", foreignField: "blob", as: "result" }
    },
];
    const unionWithVSThenLookupExpected = [
        {"_id": 100, "localField": "cakes", "weird": false, "result": []},
        {
            "_id": 101,
            "localField": "cakes and kale",
            "weird": true,
            "result": [{"_id": 1, "blob": true}, {"_id": 2, "blob": true}]
        },
        {"title": "Tarzan the Ape Man", "score": 1, "weird": false, "result": []},
        {
            "title": "King Kong",
            "score": 0.8,
            "weird": true,
            "result": [{"_id": 1, "blob": true}, {"_id": 2, "blob": true}]
        }
    ];

    setupVectorSearchResponse();

    const unionWithVSThenLookupResult = basicColl.aggregate(unionWithVSThenLookup).toArray();
    assert.eq(unionWithVSThenLookupResult, unionWithVSThenLookupExpected);
})();

// Failure Case: The query should fail when $vectorSearch is not the first stage of the
// $unionWith pipeline (because $vectorSearch must be the first stage of any pipeline
// it's in).)
const unionWithMatchVS = [{
    $unionWith: {
        coll: vectorSearchCollName,
        pipeline: [{$match: {title: "Titanic"}}].concat(vsPipeline),
    }
}];
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: basicCollName, pipeline: unionWithMatchVS, cursor: {}}), 40602);

mongotMock.stop();
MongoRunner.stopMongod(conn);