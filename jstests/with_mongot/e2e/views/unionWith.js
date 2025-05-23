/**
 * This file uses $unionWith to join two $search aggregations on a combination of views and
 * collections. The purpose of which is to test running $search on mongot-indexed views. Each of the
 * three test cases inspects explain output for execution pipeline correctness.
 * 1. Outer collection and inner view.
 * 2. Outer view and inner collection.
 * 3. Outer collection and inner view.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    assertUnionWithSearchSubPipelineAppliedViews
} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const bestPictureColl = testDb.best_picture;
const bestActressColl = testDb.best_actress;
bestPictureColl.drop();
bestActressColl.drop();

assert.commandWorked(bestActressColl.insertMany([
    {title: "Klute", year: 1971, recipient: "Jane Fonda"},
    {title: "Cabaret", year: 1972, recipient: "Liza Minnelli"},
    {title: "Network", year: 1976, recipient: "Faye Dunaway"},
    {title: "Norma Rae", year: 1979, recipient: "Sally Field"},
    {title: "Sophie's Choice", year: 1982, recipient: "Meryl Streep"},
    {title: "Terms of Endearment", year: 1983, recipient: "Shirly MacLaine"},
    {title: "Moonstruck", year: 1987, recipient: "Cher"},
    {title: "As Good as It Gets", year: 1997, recipient: "Helen Hunt"}
]));
let viewName = "bestActressAwardsAfter1979";
let bestActressViewPipeline =
    [{"$match": {"$expr": {'$and': [{'$gt': ['$year', 1979]}, {'$lt': ['$year', 1997]}]}}}];
assert.commandWorked(
    testDb.createView(viewName, bestActressColl.getName(), bestActressViewPipeline));
let bestActressView = testDb[viewName];

assert.commandWorked(bestPictureColl.insertMany([
    {title: "The French Connection", year: 1971, rotten_tomatoes_score: "96%"},
    {title: "The Godfather", year: 1972},
    {title: "Rocky", year: 1976},
    {title: "Kramer vs Kramer", year: 1979, rotten_tomatoes_score: "90%"},
    {title: "Gandhi", year: 1982, rotten_tomatoes_score: "89%"},
    {title: "Terms of Endearment", year: 1983},
    {title: "The Last Emperor", year: 1987},
    {title: "As Good as It Gets", year: 1997}
]));
viewName = "bestPictureAwardsWithRottenTomatoScore";
let bestPicturesViewPipeline =
    [{"$addFields": {rotten_tomatoes_score: {$ifNull: ['$rotten_tomatoes_score', '62%']}}}];
assert.commandWorked(
    testDb.createView(viewName, bestPictureColl.getName(), bestPicturesViewPipeline));
let bestPictureView = testDb[viewName];

// Create search indexes for all collections and views.
const indexConfigs = [
    {
        coll: bestActressView,
        definition: {name: "default", definition: {"mappings": {"dynamic": true}}}
    },
    {
        coll: bestPictureView,
        definition: {name: "default", definition: {"mappings": {"dynamic": true}}}
    },
    {
        coll: bestActressColl,
        definition: {name: "bestActressCollIndex", definition: {"mappings": {"dynamic": true}}}
    },
    {
        coll: bestPictureColl,
        definition: {name: "bestPictureCollIndex", definition: {"mappings": {"dynamic": true}}}
    }
];

const unionWithTestCases = (isStoredSource) => {
    // ===============================================================================
    // Case 1: $unionWith on outer and inner views.
    // ===============================================================================
    let pipeline = [
        {
            $search: {
                text: {query: 'Terms of Endearment', path: 'title'},
                returnStoredSource: isStoredSource
            }
        },
        {$set: {source: bestActressView.getName()}},
        {$project: {_id: 0}},
        {
            $unionWith: {
                coll: bestPictureView.getName(),
                pipeline: [
                    {
                        $search: {
                            text: {query: 'Terms of Endearment', path: 'title'},
                            returnStoredSource: isStoredSource
                        }
                    },
                    {$set: {source: bestPictureView.getName()}},
                    {$project: {_id: 0}},
                ]
            }
        }
    ];

    let expectedResults = [
        {
            title: "Terms of Endearment",
            year: 1983,
            recipient: "Shirly MacLaine",
            source: "bestActressAwardsAfter1979"
        },
        {
            title: "Terms of Endearment",
            year: 1983,
            rotten_tomatoes_score: "62%",
            source: "bestPictureAwardsWithRottenTomatoScore"
        }
    ];

    validateSearchExplain(
        bestActressView, pipeline, isStoredSource, bestActressViewPipeline, (explain) => {
            assertUnionWithSearchSubPipelineAppliedViews(explain,
                                                         bestPictureColl,
                                                         bestPictureView,
                                                         bestPicturesViewPipeline,
                                                         isStoredSource);
        });

    let results = bestActressView.aggregate(pipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // ===============================================================================
    // Case 2: $unionWith on an outer view and inner collection.
    // ===============================================================================
    pipeline = [
        {
            $search: {
                text: {query: 'Terms of Endearment', path: 'title'},
                returnStoredSource: isStoredSource
            }
        },
        {$set: {source: bestActressView.getName()}},
        {$project: {_id: 0}},
        {
            $unionWith: {
                coll: bestPictureColl.getName(),
                pipeline: [
                    {
                        $search: {
                            index: "bestPictureCollIndex",
                            text: {query: 'Terms of Endearment', path: 'title'},
                            returnStoredSource: isStoredSource
                        }
                    },
                    {$set: {source: bestPictureColl.getName()}},
                    {$project: {_id: 0}},
                ]
            }
        }
    ];

    expectedResults = [
        {
            title: "Terms of Endearment",
            year: 1983,
            recipient: "Shirly MacLaine",
            source: "bestActressAwardsAfter1979"
        },
        {title: "Terms of Endearment", year: 1983, source: "best_picture"}
    ];

    validateSearchExplain(bestActressView, pipeline, isStoredSource, bestActressViewPipeline);

    results = bestActressView.aggregate(pipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // ===============================================================================
    // Case 3: $unionWith on an outer collection and inner view.
    // ===============================================================================
    let unionWithSubPipe = [
        {
            $search: {
                text: {query: 'As Good as It Gets', path: 'title'},
                returnStoredSource: isStoredSource
            }
        },
        {$set: {source: bestPictureView.getName()}},
        {$project: {_id: 0}}
    ];

    pipeline = [
        {
            $search: {
                index: "bestActressCollIndex",
                text: {query: 'As Good as It Gets', path: 'title'},
                returnStoredSource: isStoredSource
            }
        },
        {$set: {source: bestActressColl.getName()}},
        {$project: {_id: 0}},
        {$unionWith: {coll: bestPictureView.getName(), pipeline: unionWithSubPipe}}
    ];

    expectedResults = [
        {title: "As Good as It Gets", year: 1997, recipient: "Helen Hunt", source: "best_actress"},
        {
            title: "As Good as It Gets",
            year: 1997,
            rotten_tomatoes_score: "62%",
            source: "bestPictureAwardsWithRottenTomatoScore"
        }
    ];

    validateSearchExplain(bestActressColl,
                          pipeline,
                          isStoredSource,
                          null,  // No view pipeline to verify application of since we are running
                                 // on a collection.
                          (explain) => {
                              assertUnionWithSearchSubPipelineAppliedViews(explain,
                                                                           bestPictureColl,
                                                                           bestPictureView,
                                                                           bestPicturesViewPipeline,
                                                                           isStoredSource);
                          });

    results = bestActressColl.aggregate(pipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});
};

createSearchIndexesAndExecuteTests(indexConfigs, unionWithTestCases);
