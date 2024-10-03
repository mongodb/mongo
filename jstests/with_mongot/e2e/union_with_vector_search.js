/**
 * Testing $unionWith: {$vectorSearch}
 * The collection used for $vectorSearch in this test includes no score ties.
 */

import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {
    getMovieData,
    getPlotEmbeddingById
} from "jstests/with_mongot/e2e/lib/hybrid_scoring_data.js";
import {assertDocArrExpectedFuzzy} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

const moviesCollName = jsTestName() + "_movies";
const moviesColl = db.getCollection(moviesCollName);
moviesColl.drop();

assert.commandWorked(moviesColl.insertMany(getMovieData()));

// Index is blocking by default so that the query is only run after index has been made.
moviesColl.createSearchIndex(
    {name: "search_movie_block", definition: {"mappings": {"dynamic": true}}});

// Create vector search index on movie plot embeddings.
const vectorIndex = {
    name: "vector_search_movie_block",
    type: "vectorSearch",
    definition: {
        "fields": [{
            "type": "vector",
            "numDimensions": 1536,
            "path": "plot_embedding",
            "similarity": "euclidean"
        }]
    }
};
moviesColl.createSearchIndex(vectorIndex);

// Creating a basic collection to $unionWith with the vector search collection.
const basicCollName = jsTestName() + "_basic";
const basicColl = db.getCollection(basicCollName);
basicColl.drop();

assert.commandWorked(basicColl.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(basicColl.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

const limit = 20;  // Default limit (number of documents returned)
const vectorSearchOverrequestFactor =
    10;  // Multiplication factor of k for numCandidates in $vectorSearch.

// Takes "nReturned" argument to control how many documents are returned by $vectorSearch
// ("nReturned" also affects the number of candidates considered here).
function getVSQuery(nReturned) {
    let query = [
        {
            $vectorSearch: {
                queryVector: getPlotEmbeddingById(6),  // embedding for 'Tarzan the Ape Man'
                path: "plot_embedding",
                numCandidates: nReturned * vectorSearchOverrequestFactor,
                index: "vector_search_movie_block",
                limit: nReturned,
            }
        },
    ];
    return query;
}

// Common use case of $vectorSearch + basic projection.
function getVSPipeline(nReturned) {
    return getVSQuery(nReturned).concat([
        {$project: {_id: 0, title: 1, score: {$meta: 'vectorSearchScore'}}},
    ]);
}

// Basic check of an isolated $vectorSearch.
const vectorSearchExpected = [
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "The Son of Kong", "score": 0.7921581864356995},
    {"title": "Planet of the Apes", "score": 0.7871038317680359},
    {"title": "Dawn of the Planet of the Apes", "score": 0.7863630652427673},
    {"title": "Hatari!", "score": 0.7841007709503174},
    {"title": "Journey to the Center of the Earth", "score": 0.783767819404602},
    {"title": "George of the Jungle", "score": 0.7831006050109863},
    {"title": "Battle for the Planet of the Apes", "score": 0.7793492078781128},
    {"title": "Beauty and the Beast", "score": 0.7619031071662903},
    {"title": "Rise of the Planet of the Apes", "score": 0.7488002181053162},
    {"title": "King Kong Lives", "score": 0.7352674007415771},
    {"title": "Kung Fu Panda", "score": 0.7324357032775879},
    {"title": "Abraham Lincoln: Vampire Hunter", "score": 0.6931257247924805},
    {"title": "Titanic", "score": 0.6765283942222595}
];
var vectorSearchResult = moviesColl.aggregate(getVSPipeline(15)).toArray();
assert.fuzzySameMembers(vectorSearchResult, vectorSearchExpected, ["score"]);

// Basic collection $unionWith $vectorSearch.
var basicUnionWith = [{
    $unionWith: {
        coll: moviesCollName,
        pipeline: getVSPipeline(30),
    }
}];
const basicUnionWithExpected = [
    {"_id": 100, "localField": "cakes", "weird": false},
    {"_id": 101, "localField": "cakes and kale", "weird": true},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "The Son of Kong", "score": 0.7921581864356995},
    {"title": "Planet of the Apes", "score": 0.7871038317680359},
    {"title": "Dawn of the Planet of the Apes", "score": 0.7863630652427673},
    {"title": "Hatari!", "score": 0.7841007709503174},
    {"title": "Journey to the Center of the Earth", "score": 0.783767819404602},
    {"title": "George of the Jungle", "score": 0.7831006050109863},
    {"title": "Battle for the Planet of the Apes", "score": 0.7793492078781128},
    {"title": "Beauty and the Beast", "score": 0.7619031071662903},
    {"title": "Rise of the Planet of the Apes", "score": 0.7488002181053162},
    {"title": "King Kong Lives", "score": 0.7352674007415771},
    {"title": "Kung Fu Panda", "score": 0.7324357032775879},
    {"title": "Abraham Lincoln: Vampire Hunter", "score": 0.6931257247924805},
    {"title": "Titanic", "score": 0.6765283942222595}

];
var basicUnionWithResult = basicColl.aggregate(basicUnionWith).toArray();
assert.fuzzySameMembers(basicUnionWithResult, basicUnionWithExpected, ["score"]);

// Check that the explain output of the above contains both $unionWith and $vectorSearch.
// "queryPlanner" is currently the only supported verbosity option.
const result = basicColl.explain("queryPlanner").aggregate(basicUnionWith);
assert.neq(getAggPlanStages(result, "$unionWith"), null, result);
assert.neq(getAggPlanStages(result, "$vectorSearch"), null, result);

// $vectorSearch on moviesColl $unionWith $vectorSearch on moviesColl.
var vsUnionWithVs1 = getVSPipeline(2);
var vsUnionWithVs2 = [
    {$project: {_id: 0, title: 1, score: {$meta: 'vectorSearchScore'}}},
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        }
    }
];
var vsUnionWithVs = vsUnionWithVs1.concat(vsUnionWithVs2);
const vsUnionWithVsExpected = [
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
var vsUnionWithVsResult = moviesColl.aggregate(vsUnionWithVs).toArray();
assert.fuzzySameMembers(vsUnionWithVsResult, vsUnionWithVsExpected, ["score"]);

// Creating moviesCollB, which is another collection with a vector search index
// (has the same data as moviesColl)
const moviesCollBName = jsTestName() + "_movies_B";
const moviesCollB = db.getCollection(moviesCollBName);
moviesCollB.drop();
assert.commandWorked(moviesCollB.insertMany(getMovieData()));
moviesCollB.createSearchIndex(
    {name: "search_movie_block", definition: {"mappings": {"dynamic": true}}});

// $vectorSearch on moviesColl $unionWith $vectorSearch on moviesCollB.
var vsUnionWithVsCollB = getVSPipeline(2).concat([{
    $unionWith: {
        coll: moviesCollName,
        pipeline: getVSPipeline(3),
    }
}]);
const vsUnionWithVsCollBExpected = [
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "The Son of Kong", "score": 0.7921581864356995},
];
var vsUnionWithVsCollBResult = moviesColl.aggregate(vsUnionWithVsCollB).toArray();
assert.fuzzySameMembers(vsUnionWithVsCollBResult, vsUnionWithVsCollBExpected, ["score"]);

// Multiple $unionWith $vectorSearch stages:  {$unionWith $vectorSearch}, {$unionWith
// $vectorSearch}.
var dualUnionWith = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        }
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        }
    },
];
const dualUnionWithExpected = [
    {"_id": 100, "localField": "cakes", "weird": false},
    {"_id": 101, "localField": "cakes and kale", "weird": true},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
var dualUnionWithResult = basicColl.aggregate(dualUnionWith).toArray();
assert.fuzzySameMembers(dualUnionWithResult, dualUnionWithExpected, ["score"]);

// Nested $unionWith $vectorSearch (should get same results as above).
var nestedUnionWith = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2).concat([
                {
                    $unionWith: {
                        coll: moviesCollName,
                        pipeline: getVSPipeline(2),
                    }
                },
            ]),
        }
    },
];
var nestedUnionWithResult = basicColl.aggregate(nestedUnionWith).toArray();
assert.fuzzySameMembers(nestedUnionWithResult, dualUnionWithExpected, ["score"]);

// $unionWith {$vectorSearch} after a non-$vectorSearch $unionWith, and vice versa.
var twoDiffUnionWith = [
    {
        $unionWith: {
            coll: basicCollName,
        }
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        }
    },
];
const twoDiffUnionWithExpected = [
    {"_id": 100, "localField": "cakes", "weird": false},
    {"_id": 101, "localField": "cakes and kale", "weird": true},
    {"_id": 100, "localField": "cakes", "weird": false},
    {"_id": 101, "localField": "cakes and kale", "weird": true},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
var twoDiffUnionWithResult = basicColl.aggregate(twoDiffUnionWith).toArray();
assert.fuzzySameMembers(twoDiffUnionWithResult, twoDiffUnionWithExpected, ["score"]);

var twoDiffUnionWithFlipped = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
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
    {"title": "King Kong", "score": 0.8097645044326782},
    {"_id": 100, "localField": "cakes", "weird": false},
    {"_id": 101, "localField": "cakes and kale", "weird": true},
];
var twoDiffUnionWithFlippedResult = basicColl.aggregate(twoDiffUnionWithFlipped).toArray();
assert.fuzzySameMembers(twoDiffUnionWithFlippedResult, twoDiffUnionWithFlippedExpected, ["score"]);

// Match stage after a $unionWith $vectorSearch.
var matchAfterUnionWith = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(15),
        }
    },
    {$match: {title: "Titanic"}},
];
const matchAfterUnionWithExpected = [
    {"title": "Titanic", "score": 0.6765283942222595},
];
var matchAfterUnionWithResult = basicColl.aggregate(matchAfterUnionWith).toArray();
assert.fuzzySameMembers(matchAfterUnionWithResult, matchAfterUnionWithExpected, ["score"]);

// $addFields to basic collection (basicColl) prior to a $unionWith $vectorSearch.
var addFieldsToBasicPreUnionWith = [
    {
        $addFields: {
            id2: "$_id",
        }
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        }
    },
];
const addFieldsToBasicPreUnionWithExpected = [
    {"_id": 100, "localField": "cakes", "weird": false, "id2": 100},
    {"_id": 101, "localField": "cakes and kale", "weird": true, "id2": 101},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
var addFieldsToBasicPreUnionWithResult =
    basicColl.aggregate(addFieldsToBasicPreUnionWith).toArray();
assert.fuzzySameMembers(
    addFieldsToBasicPreUnionWithResult, addFieldsToBasicPreUnionWithExpected, ["score"]);

//$addFields to movie data collection (coll) prior to a $unionWith $vectorSearch.
var addFieldsToMoviesPreUnionWith = [
    {
        $sort: {_id: 1},
    },
    {
        $limit: 2,
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
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        }
    },
];
const addFieldsToMoviesPreUnionWithExpected = [
    {"_id": 0, "title": "It's a Mad, Mad, Mad, Mad World", "id2": 0},
    {"_id": 1, "title": "Battle for the Planet of the Apes", "id2": 1},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
var addFieldsToMoviesPreUnionWithResult =
    moviesColl.aggregate(addFieldsToMoviesPreUnionWith).toArray();
assert.fuzzySameMembers(
    addFieldsToMoviesPreUnionWithResult, addFieldsToMoviesPreUnionWithExpected, ["score"]);

// Adding (non-meta related) fields within $unionWith.
var addFieldsWithinUnionWith = [
    {
        $sort: {_id: 1},
    },
    {
        $limit: 2,
    },
    {
        $project: {title: 1},
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSQuery(2).concat([
                {$project: {title: 1, score: {$meta: 'vectorSearchScore'}}},
                {$addFields: {id2: "$_id"}},
            ]),
        }
    }
];
const addFieldsWithinUnionWithExpected = [
    {"_id": 0, "title": "It's a Mad, Mad, Mad, Mad World"},
    {"_id": 1, "title": "Battle for the Planet of the Apes"},
    {"_id": 6, "title": "Tarzan the Ape Man", "score": 1, "id2": 6},
    {"_id": 4, "title": "King Kong", "score": 0.8097645044326782, "id2": 4},
];
var addFieldsWithinUnionWithResult = moviesColl.aggregate(addFieldsWithinUnionWith).toArray();
assert.fuzzySameMembers(
    addFieldsWithinUnionWithResult, addFieldsWithinUnionWithExpected, ["score"]);

// Adding fields after $unionWith.
var addFieldsAfterUnionWith = [
    {
        $sort: {_id: 1},
    },
    {
        $limit: 2,
    },
    {
        $project: {title: 1},
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSQuery(2).concat([
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
    {"_id": 0, "title": "It's a Mad, Mad, Mad, Mad World", "id2": 0},
    {"_id": 1, "title": "Battle for the Planet of the Apes", "id2": 1},
    {"_id": 6, "title": "Tarzan the Ape Man", "score": 1, "id2": 6},
    {"_id": 4, "title": "King Kong", "score": 0.8097645044326782, "id2": 4},
];
var addFieldsAfterUnionWithResult = moviesColl.aggregate(addFieldsAfterUnionWith).toArray();
assert.fuzzySameMembers(addFieldsAfterUnionWithResult, addFieldsAfterUnionWithExpected, ["score"]);

// Sorting by score AFTER a unionWith.
var sortAfterUnionWith = [
    {
        $addFields: {score: 2},
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSQuery(2).concat([
                {$project: {_id: 1, title: 1, score: {$meta: 'vectorSearchScore'}}},
            ]),
        }
    },
    {$sort: {score: -1}},
];
const sortAfterUnionWithExpected = [
    {"_id": 100, "localField": "cakes", "weird": false, "score": 2},
    {"_id": 101, "localField": "cakes and kale", "weird": true, "score": 2},
    {"_id": 6, "title": "Tarzan the Ape Man", "score": 1},
    {"_id": 4, "title": "King Kong", "score": 0.8097645044326782},
];
var sortAfterUnionWithResult = basicColl.aggregate(sortAfterUnionWith).toArray();
assertDocArrExpectedFuzzy(sortAfterUnionWithResult, sortAfterUnionWithExpected);

// Set up to test with a $lookup stage.
const lookupCollName = "union_with_vector_search_lookup";
const lookupColl = db.getCollection(lookupCollName);
lookupColl.drop();

assert.commandWorked(lookupColl.insert({_id: 1, blob: true}));
assert.commandWorked(lookupColl.insert({_id: 2, blob: true}));

// {$lookup (non-VS)}, {$unionWith $vectorSearch}.
var lookupThenUnionWithVS = [
    {
        $lookup:
            {from: lookupColl.getName(), localField: "weird", foreignField: "blob", as: "result"}

    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
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
    {"title": "King Kong", "score": 0.8097645044326782}
];
var lookupThenUnionWithVSResult = basicColl.aggregate(lookupThenUnionWithVS).toArray();
assert.fuzzySameMembers(lookupThenUnionWithVSResult, lookupThenUnionWithVSExpected, ["score"]);

// {$unionWith $vectorSearch}, {$lookup (non-VS)}.
var unionWithVSThenLookup = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2).concat([
                {$addFields: {"weird": {$lt: ["$score", 1]}}},
            ]),
        }
    },
    {
        $lookup:
            {from: lookupColl.getName(), localField: "weird", foreignField: "blob", as: "result"}
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
        "score": 0.8097645044326782,
        "weird": true,
        "result": [{"_id": 1, "blob": true}, {"_id": 2, "blob": true}]
    }
];
var unionWithVSThenLookupResult = basicColl.aggregate(unionWithVSThenLookup).toArray();
assert.fuzzySameMembers(unionWithVSThenLookupResult, unionWithVSThenLookupExpected, ["score"]);

// Failure Case: The query should fail when $vectorSearch is not the first stage of the
// $unionWith pipeline (because $vectorSearch must be the first stage of any pipeline
// it's in).)
var unionWithMatchVS = [{
    $unionWith: {
        coll: moviesCollName,
        pipeline: [{$match: {title: "Titanic"}}].concat(getVSPipeline(2)),
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: basicCollName, pipeline: unionWithMatchVS, cursor: {}}), 40602);
