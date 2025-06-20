/**
 * This test verifies that when a $search stage specifies an index that does not exist on the
 * top-level aggregation namespace or only exists on the underlying collection, the query returns no
 * results.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    createSearchIndexesAndExecuteTests,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const bestPictureColl = testDb.best_picture;
bestPictureColl.drop();

const tvShowColl = testDb.tvShowColl;
tvShowColl.drop();
tvShowColl.insertOne({title: "Breaking Bad"});

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
const viewName = "bestPictureAwardsWithRottenTomatoScore";
const bestPicturesViewPipeline =
    [{"$addFields": {rotten_tomatoes_score: {$ifNull: ['$rotten_tomatoes_score', '62%']}}}];
assert.commandWorked(
    testDb.createView(viewName, bestPictureColl.getName(), bestPicturesViewPipeline));
const bestPictureView = testDb[viewName];

// Create two indexes on bestPictureColl, none on bestPictureView.
const indexConfigs = [
    {
        coll: bestPictureColl,
        definition: {name: "default", definition: {"mappings": {"dynamic": true}}}
    },
    {
        coll: bestPictureColl,
        definition: {name: "bestPictureCollIndex", definition: {"mappings": {"dynamic": true}}}
    }
];

const undefinedOrUnderlyingIndexTestCases = (isStoredSource) => {
    const basicQueryResult = (indexName) => {
        return bestPictureView
            .aggregate({
                $search: {
                    index: indexName,
                    text: {query: 'emperor', path: 'title'},
                    returnStoredSource: isStoredSource
                }
            })
            .toArray();
    };

    const unionWithQueryResult = (indexName) => {
        return tvShowColl
            .aggregate([
                {$project: {_id: 0}},
                {
                    $unionWith: {
                        coll: bestPictureView.getName(),
                        pipeline: [{
                            $search: {
                                index: indexName,
                                text: {query: 'emperor', path: 'title'},
                                returnStoredSource: isStoredSource
                            }
                        }]
                    }
                }
            ])
            .toArray();
    };

    const lookupQueryResult = (indexName) => {
        return bestPictureColl
            .aggregate([
                {
                    $lookup: {
                        from: bestPictureView.getName(),
                        pipeline: [
                            {
                                $search: {
                                    index: indexName,
                                    text: {query: 'emperor', path: 'title'},
                                    returnStoredSource: isStoredSource
                                }
                            },
                        ],
                        as: "bestPictureView"
                    }
                },
                {
                    // Filter out documents where the lookup did not return any results (should be 
                    // all of the documents).
                    $match: {
                        bestPictureView: {$ne: []}
                    }
                }
            ])
            .toArray();
    };

    const indexNames = ["default", "bestPictureCollIndex", "thisIsNotAnIndex"];

    for (const indexName of indexNames) {
        jsTestLog(`Testing index: ${indexName} with stored source: ${isStoredSource}`);
        assertArrayEq({actual: basicQueryResult(indexName), expected: []});
        assertArrayEq(
            {actual: unionWithQueryResult(indexName), expected: [{title: "Breaking Bad"}]});
        assertArrayEq({actual: lookupQueryResult(indexName), expected: []});
    }
};

createSearchIndexesAndExecuteTests(indexConfigs, undefinedOrUnderlyingIndexTestCases);
