/**
 * This file tests a $unionWith + $lookup combination across a view and collection respectively. The
 * purpose is to verify that running a $search operation on a $unionWith in such a situation
 * returns the correct results.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    checkSbeFullyEnabled,
} from "jstests/libs/query/sbe_util.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    assertLookupInExplain,
    assertUnionWithSearchSubPipelineAppliedViews,
    assertViewAppliedCorrectly
} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const bestPictureColl = db["best_picture"];
const bestActressColl = db["best_actress"];
bestPictureColl.drop();
bestActressColl.drop();

const isSbeFullyEnabled = checkSbeFullyEnabled(db);

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
    [{"$match": {"$expr": {'$and': [{'$gt': ['$year', 1979]}, {'$lt': ['$year', 1997]}]}}}];
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
    [{"$addFields": {rotten_tomatoes_score: {$ifNull: ['$rotten_tomatoes_score', '62%']}}}];
assert.commandWorked(db.createView(viewName, bestPictureColl.getName(), bestPicturesViewPipeline));
const bestPictureView = db[viewName];
createSearchIndex(bestPictureView, {name: "default", definition: {"mappings": {"dynamic": true}}});

// $lookup and $unionWith.
// Create nominations collection to call $lookup on.
const nominationsColl = db["nominations"];
nominationsColl.drop();

assert.commandWorked(nominationsColl.insertMany([
    {title: "Terms of Endearment", year: 1983, category: "Best Picture"},
    {title: "Moonstruck", year: 1987, category: "Best Picture"}
]));

const lookupStage = {
    $lookup: {
        from: nominationsColl.getName(),
        localField: "title",
        foreignField: "title",
        as: "nominations"
    }
};

const pipeline = [
    // Join with the nomination's collection based on title.
    lookupStage,
    // Flatten results.
    {$unwind: "$nominations"},
    {$project: {_id: 0, "nominations.category": 1, title: 1, year: 1}},
    {
        $unionWith: {
            coll: bestPictureView.getName(),
            pipeline: [
                // Search bestPictureView.
                {$search: {text: {query: 'Terms of Endearment', path: 'title'}}},
                {$set: {source: bestPictureView.getName()}},
                {$project: {_id: 0}}
            ]
        }
    }
];

const expectedResults = [
    {
        title: "Terms of Endearment",
        year: 1983,
        nominations: {category: "Best Picture"},
    },
    {title: "Moonstruck", year: 1987, nominations: {category: "Best Picture"}},
    {
        title: "Terms of Endearment",
        year: 1983,
        rotten_tomatoes_score: "62%",
        source: "bestPictureAwardsWithRottenTomatoScore"
    }
];

// Assert that the outer view definition is applied correctly, the $unionWith view defintion is
// applied correctly, and the lookup stage exists in the explain output.
const explain = assert.commandWorked(bestActressView.explain().aggregate(pipeline));
assertViewAppliedCorrectly(explain, pipeline, bestActressViewPipeline[0]);
assertUnionWithSearchSubPipelineAppliedViews(
    explain, bestPictureColl, bestPictureView, bestPicturesViewPipeline);

// $lookup won't exist in the explain output if SBE is fully enabled.
if (!isSbeFullyEnabled || FixtureHelpers.numberOfShardsForCollection(bestActressColl) > 1) {
    assertLookupInExplain(explain, lookupStage);
}

const results = bestActressView.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});

dropSearchIndex(bestActressView, {name: "default"});
dropSearchIndex(bestPictureView, {name: "default"});
