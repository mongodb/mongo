/**
 * This file tests nested $unionWith pipelines involving $search operations across views. The
 * purpose is to verify that the nested unions and searches return the correct results across all
 * views.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    assertUnionWithSearchSubPipelineAppliedViews,
    assertViewAppliedCorrectly
} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const bestPictureColl = db["best_picture"];
const bestActressColl = db["best_actress"];
const bestActorColl = db["best_actor"];
const bestDirectorColl = db["best_director"];
bestPictureColl.drop();
bestActressColl.drop();
bestActorColl.drop();
bestDirectorColl.drop();

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
const bestActressViewPipeline =
    [{"$match": {"$expr": {"$and": [{"$gt": ["$year", 1979]}, {"$lt": ["$year", 1997]}]}}}];
assert.commandWorked(db.createView(viewName, bestActressColl.getName(), bestActressViewPipeline));
const bestActressView = db[viewName];
createSearchIndex(bestActressView, {name: "default", definition: {"mappings": {"dynamic": true}}});

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
const bestPicturesViewPipeline =
    [{"$addFields": {rotten_tomatoes_score: {$ifNull: ["$rotten_tomatoes_score", "62%"]}}}];
assert.commandWorked(db.createView(viewName, bestPictureColl.getName(), bestPicturesViewPipeline));
const bestPictureView = db[viewName];
createSearchIndex(bestPictureView, {name: "default", definition: {"mappings": {"dynamic": true}}});
assert.commandWorked(bestActorColl.insertMany([
    {title: "The French Connection", year: 1971, recipient: "Gene Hackman"},
    {title: "The Godfather", year: 1972, recipient: "Marlon Brando"},
    {title: "Network", year: 1976, recipient: "Peter Finch"},
    {title: "Kramer vs Kramer", year: 1979, recipient: "Dustin Hoffman"},
    {title: "Gandhi", year: 1982, recipient: "Ben Kingsley"},
    {title: "Tender Mercies", year: 1983, recipient: "Robert Duvall"},
    {title: "Wall Street", year: 1987, recipient: "Michael Douglas"},
    {title: "As Good as It Gets", year: 1997, recipient: "Jack Nicholson"},
]));
viewName = "bestActorAwardsBefore1979";
const bestActorViewPipeline =
    [{"$match": {"$expr": {"$and": [{"$gt": ["$year", 1970]}, {"$lt": ["$year", 1979]}]}}}];
assert.commandWorked(db.createView(viewName, bestActorColl.getName(), bestActorViewPipeline));
const bestActorView = db[viewName];
createSearchIndex(bestActorView, {name: "default", definition: {"mappings": {"dynamic": true}}});

assert.commandWorked(bestDirectorColl.insertMany([
    {title: "The French Connection", year: 1971, recipient: "William Friedkin"},
    {title: "Cabaret", year: 1972, recipient: "Bob Fosse"},
    {title: "Rocky", year: 1976, recipient: "John G. Avildsen"},
    {title: "Kramer vs Kramer", year: 1979, recipient: "Robert Benton"},
    {
        title: "Gandhi",
        year: 1982,
        recipient: "Richard Attenborough",
        best_original_screenplay: true
    },
    {title: "Terms of Endearment", year: 1983, recipient: "James L. Brooks"},
    {title: "The Last Emperor", year: 1987, recipient: "Bernardo Bertolucci"},
    {title: "Titanic", year: 1997, recipient: "James Cameron"},
]));
viewName = "bestDirectorAwardsWithBestOriginalScreenplay";
const bestDirectorViewPipeline =
    [{"$addFields": {best_original_screenplay: {$ifNull: ["$best_original_screenplay", false]}}}];
assert.commandWorked(db.createView(viewName, bestDirectorColl.getName(), bestDirectorViewPipeline));
const bestDirectorView = db[viewName];
createSearchIndex(bestDirectorView, {name: "default", definition: {"mappings": {"dynamic": true}}});

// Deep nested $unionWith.
const pipeline = [
    {$set: {source: bestActressView.getName() + "_outer"}},
    {
        $unionWith: {
            coll: bestPictureView.getName(),
            pipeline: [
                // Search bestPictureView.
                {$search: {text: {query: "Terms of Endearment", path: "title"}}},
                {$set: {source: bestPictureView.getName() + "_inner"}},
                {
                    $unionWith: {
                        coll: bestActressView.getName(),
                        pipeline: [
                            // Search bestActressView.
                            {$search: {text: {query: "Terms of Endear", path: "title"}}},
                            {$set: {source: bestActressView.getName() + "_inner"}},
                            {
                                $unionWith: {
                                    coll: bestActorView.getName(),
                                    pipeline: [
                                        // Search bestActorView.
                                        {$search: {text: {query: "Network", path: "title"}}},
                                        {$set: {source: bestActorView.getName() + "_inner"}},
                                        {
                                            $unionWith: {
                                                coll: bestDirectorView.getName(),
                                                pipeline: [
                                                    // Search bestDirectorView.
                                                    {
                                                        $search: {
                                                            text: {query: "Titanic", path: "title"}
                                                        }
                                                    },
                                                    {
                                                        $set: {
                                                            source: bestDirectorView.getName() +
                                                                "_inner"
                                                        }
                                                    }
                                                ]
                                            }
                                        }
                                    ]
                                },
                            }
                        ]
                    }
                }
            ]
        }
    },
    {$project: {_id: 0}}
];

const expectedResults = [
    {
        title: "Sophie's Choice",
        year: 1982,
        recipient: "Meryl Streep",
        source: "bestActressAwardsAfter1979_outer"
    },
    {
        title: "Terms of Endearment",
        year: 1983,
        recipient: "Shirly MacLaine",
        source: "bestActressAwardsAfter1979_outer"
    },
    {
        title: "Moonstruck",
        year: 1987,
        recipient: "Cher",
        source: "bestActressAwardsAfter1979_outer"
    },
    {
        title: "Terms of Endearment",
        year: 1983,
        rotten_tomatoes_score: "62%",
        source: "bestPictureAwardsWithRottenTomatoScore_inner"
    },
    {
        title: "Terms of Endearment",
        year: 1983,
        recipient: "Shirly MacLaine",
        source: "bestActressAwardsAfter1979_inner"
    },
    {
        title: "Network",
        year: 1976,
        recipient: "Peter Finch",
        source: "bestActorAwardsBefore1979_inner"
    },
    {
        title: "Titanic",
        year: 1997,
        recipient: "James Cameron",
        best_original_screenplay: false,
        source: "bestDirectorAwardsWithBestOriginalScreenplay_inner"
    }
];

const explain = assert.commandWorked(bestActressView.explain().aggregate(pipeline));
assertViewAppliedCorrectly(explain, pipeline, bestActressViewPipeline[0]);
assertUnionWithSearchSubPipelineAppliedViews(
    explain, bestPictureColl, bestPictureView, bestPicturesViewPipeline);

const results = bestActressView.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});

dropSearchIndex(bestActressView, {name: "default"});
dropSearchIndex(bestPictureView, {name: "default"});
dropSearchIndex(bestActorView, {name: "default"});
dropSearchIndex(bestDirectorView, {name: "default"});
