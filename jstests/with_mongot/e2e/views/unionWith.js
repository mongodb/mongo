/**
 * This file uses $unionWith to join two $search aggregations on a combination of views and
 * collections. The purpose of which is to test running $search on mongot-indexed views. Each of the
 * three test cases inspects explain output for execution pipeline correctness.
 * 1. outer collection and inner view.
 * 2. outer view and inner collection.
 * 3. outer collection and inner view.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    assertUnionWithSearchPipelinesApplyViews,
    assertViewAppliedCorrectly,
    extractUnionWithSubPipelineExplainOutput
} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const bestPictureColl = db["best_picture"];
const bestActressColl = db["best_actress"];
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
assert.commandWorked(db.createView(viewName, bestActressColl.getName(), bestActressViewPipeline));
let bestActressView = db[viewName];
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
let bestPicturesViewPipeline =
    [{"$addFields": {rotten_tomatoes_score: {$ifNull: ['$rotten_tomatoes_score', '62%']}}}];
assert.commandWorked(db.createView(viewName, bestPictureColl.getName(), bestPicturesViewPipeline));
let bestPictureView = db[viewName];
createSearchIndex(bestPictureView, {name: "default", definition: {"mappings": {"dynamic": true}}});

// $unionWith on outer and inner views.
let pipeline = [
    {$search: {text: {query: 'Terms of Endearment', path: 'title'}}},
    {$set: {source: bestActressView.getName()}},
    {$project: {_id: 0}},
    {
        $unionWith: {
            coll: bestPictureView.getName(),
            pipeline: [
                {$search: {text: {query: 'Terms of Endearment', path: 'title'}}},
                {$set: {source: bestPictureView.getName()}},
                {$project: {_id: 0}},
            ]
        }
    }
];
let explain = bestActressView.explain().aggregate(pipeline);
/**
 * This call will confirm that the outer and inner search pipelines have idLookup applying the view
 * stages.
 */
assertUnionWithSearchPipelinesApplyViews(
    explain.stages, bestActressViewPipeline, bestPicturesViewPipeline);

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
let results = bestActressView.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});

//$unionWith on an outer view and inner collection.
pipeline = [
    {$search: {text: {query: 'Terms of Endearment', path: 'title'}}},
    {$set: {source: bestActressView.getName()}},
    {$project: {_id: 0}},
    {
        $unionWith: {
            coll: bestPictureColl.getName(),
            pipeline: [
                {$search: {text: {query: 'Terms of Endearment', path: 'title'}}},
                {$set: {source: bestPictureColl.getName()}},
                {$project: {_id: 0}},
            ]
        }
    }
];
/**
 * Until GA, mongot won't support an index on the parent collection and an index on a descendant
 * view having the same name, so cannot name it 'default' here.
 */
createSearchIndex(bestPictureColl,
                  {name: "bestPicture", definition: {"mappings": {"dynamic": true}}});
explain = bestActressView.explain().aggregate(pipeline).stages;
// Only the outer collection is a view, and this call will confirm that the top-level search
// contains the view in idLookup.
assertViewAppliedCorrectly(explain, pipeline, bestActressViewPipeline);

expectedResults = [
    {
        "title": "Terms of Endearment",
        "year": 1983,
        "recipient": "Shirly MacLaine",
        "source": "bestActressAwardsAfter1979"
    },
    {"title": "Terms of Endearment", "year": 1983, "source": "best_picture"}
];
results = bestActressView.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});

// $unionWith on an outer collection and inner view.
createSearchIndex(bestActressColl,
                  {name: "bestActress", definition: {"mappings": {"dynamic": true}}});
let unionWithSubPipe = [
    {$search: {text: {query: 'As Good as It Gets', path: 'title'}}},
    {$set: {source: bestPictureView.getName()}},
    {$project: {_id: 0}}
];
pipeline = [
    {
        $search: {
            index: "bestActress",
            text: {
                query: 'As Good as It Gets',
                path: 'title'
            }  // This search pipeline will query the entire bestActress collection instead of the
               // view.
        }
    },
    {$set: {source: bestActressColl.getName()}},
    {$project: {_id: 0}},
    {$unionWith: {coll: bestPictureView.getName(), pipeline: unionWithSubPipe}}
];
// Confirm $unionWith.$search subpipeline applies the view stages during idLookup.
explain = bestActressColl.explain().aggregate(pipeline).stages;
let unionWithSubPipeExplain = extractUnionWithSubPipelineExplainOutput(explain);
assertViewAppliedCorrectly(unionWithSubPipeExplain, unionWithSubPipe, bestPicturesViewPipeline);

expectedResults = [
    {
        "title": "As Good as It Gets",
        "year": 1997,
        "recipient": "Helen Hunt",
        "source": "best_actress"
    },
    {
        "title": "As Good as It Gets",
        "year": 1997,
        "rotten_tomatoes_score": "62%",
        "source": "bestPictureAwardsWithRottenTomatoScore"
    }
];
results = bestActressColl.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});

dropSearchIndex(bestActressView, {name: "default"});
dropSearchIndex(bestPictureView, {name: "default"});
dropSearchIndex(bestActressColl, {name: "bestActress"});
dropSearchIndex(bestPictureColl, {name: "bestPicture"});
