/**
 * Testing $unionWith: {$vectorSearch}
 * The collection used for $vectorSearch in this test includes no score ties.
 */

import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {assertDocArrExpectedFuzzy} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const moviesCollName = jsTestName() + "_movies";
const moviesColl = db.getCollection(moviesCollName);
moviesColl.drop();

assert.commandWorked(moviesColl.insertMany(getMovieData()));

// Index is blocking by default so that the query is only run after index has been made.
createSearchIndex(moviesColl, getMovieSearchIndexSpec());

// Create vector search index on movie plot embeddings.
createSearchIndex(moviesColl, getMovieVectorSearchIndexSpec());

// Creating a basic collection to $unionWith with the vector search collection.
const basicCollName = jsTestName() + "_basic";
const basicColl = db.getCollection(basicCollName);
basicColl.drop();

assert.commandWorked(basicColl.insert({"_id": 100, "localField": "cakes", "weird": false}));
assert.commandWorked(basicColl.insert({"_id": 101, "localField": "cakes and kale", "weird": true}));

// Make sure the shard that is going to execute the $unionWith/$lookup has up-to-date routing info.
basicColl.aggregate([{$lookup: {from: moviesCollName, pipeline: [], as: "out"}}]);

const limit = 20; // Default limit (number of documents returned)
const vectorSearchOverrequestFactor = 10; // Multiplication factor of k for numCandidates in $vectorSearch.

// Takes "nReturned" argument to control how many documents are returned by $vectorSearch
// ("nReturned" also affects the number of candidates considered here).
function getVSQuery(nReturned) {
    let query = [
        {
            $vectorSearch: {
                queryVector: getMoviePlotEmbeddingById(6), // embedding for 'Tarzan the Ape Man'
                path: "plot_embedding",
                numCandidates: nReturned * vectorSearchOverrequestFactor,
                index: getMovieVectorSearchIndexSpec().name,
                limit: nReturned,
            },
        },
    ];
    return query;
}

// Common use case of $vectorSearch + basic projection.
function getVSPipeline(nReturned) {
    return getVSQuery(nReturned).concat([{$project: {_id: 0, title: 1, score: {$meta: "vectorSearchScore"}}}]);
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
    {"title": "Titanic", "score": 0.6765283942222595},
];
let vectorSearchResult = moviesColl.aggregate(getVSPipeline(15)).toArray();
assert.fuzzySameMembers(vectorSearchResult, vectorSearchExpected, ["score"]);

// Basic collection $unionWith $vectorSearch.
let basicUnionWith = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(30),
        },
    },
];
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
    {"title": "Titanic", "score": 0.6765283942222595},
];
let basicUnionWithResult = basicColl.aggregate(basicUnionWith).toArray();
assert.fuzzySameMembers(basicUnionWithResult, basicUnionWithExpected, ["score"]);

// Check that the explain output of the above contains both $unionWith and $vectorSearch.
// "queryPlanner" is currently the only supported verbosity option.
const result = basicColl.explain("queryPlanner").aggregate(basicUnionWith);
assert.neq(getAggPlanStages(result, "$unionWith"), null, result);
assert.neq(getAggPlanStages(result, "$vectorSearch"), null, result);

// $vectorSearch on moviesColl $unionWith $vectorSearch on moviesColl.
let vsUnionWithVs1 = getVSPipeline(2);
let vsUnionWithVs2 = [
    {$project: {_id: 0, title: 1, score: {$meta: "vectorSearchScore"}}},
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
    },
];
let vsUnionWithVs = vsUnionWithVs1.concat(vsUnionWithVs2);
const vsUnionWithVsExpected = [
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
let vsUnionWithVsResult = moviesColl.aggregate(vsUnionWithVs).toArray();
assert.fuzzySameMembers(vsUnionWithVsResult, vsUnionWithVsExpected, ["score"]);

// Creating moviesCollB, which is another collection with a vector search index
// (has the same data as moviesColl)
const moviesCollBName = jsTestName() + "_movies_B";
const moviesCollB = db.getCollection(moviesCollBName);
moviesCollB.drop();
assert.commandWorked(moviesCollB.insertMany(getMovieData()));
createSearchIndex(moviesCollB, getMovieSearchIndexSpec());

// $vectorSearch on moviesColl $unionWith $vectorSearch on moviesCollB.
let vsUnionWithVsCollB = getVSPipeline(2).concat([
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(3),
        },
    },
]);
const vsUnionWithVsCollBExpected = [
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
    {"title": "The Son of Kong", "score": 0.7921581864356995},
];
let vsUnionWithVsCollBResult = moviesColl.aggregate(vsUnionWithVsCollB).toArray();
assert.fuzzySameMembers(vsUnionWithVsCollBResult, vsUnionWithVsCollBExpected, ["score"]);

// Multiple $unionWith $vectorSearch stages:  {$unionWith $vectorSearch}, {$unionWith
// $vectorSearch}.
let dualUnionWith = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
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
let dualUnionWithResult = basicColl.aggregate(dualUnionWith).toArray();
assert.fuzzySameMembers(dualUnionWithResult, dualUnionWithExpected, ["score"]);

// Nested $unionWith $vectorSearch (should get same results as above).
let nestedUnionWith = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2).concat([
                {
                    $unionWith: {
                        coll: moviesCollName,
                        pipeline: getVSPipeline(2),
                    },
                },
            ]),
        },
    },
];
let nestedUnionWithResult = basicColl.aggregate(nestedUnionWith).toArray();
assert.fuzzySameMembers(nestedUnionWithResult, dualUnionWithExpected, ["score"]);

// $unionWith {$vectorSearch} after a non-$vectorSearch $unionWith, and vice versa.
let twoDiffUnionWith = [
    {
        $unionWith: {
            coll: basicCollName,
        },
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
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
let twoDiffUnionWithResult = basicColl.aggregate(twoDiffUnionWith).toArray();
assert.fuzzySameMembers(twoDiffUnionWithResult, twoDiffUnionWithExpected, ["score"]);

let twoDiffUnionWithFlipped = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
    },
    {
        $unionWith: {
            coll: basicCollName,
        },
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
let twoDiffUnionWithFlippedResult = basicColl.aggregate(twoDiffUnionWithFlipped).toArray();
assert.fuzzySameMembers(twoDiffUnionWithFlippedResult, twoDiffUnionWithFlippedExpected, ["score"]);

// Match stage after a $unionWith $vectorSearch.
let matchAfterUnionWith = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(15),
        },
    },
    {$match: {title: "Titanic"}},
];
const matchAfterUnionWithExpected = [{"title": "Titanic", "score": 0.6765283942222595}];
let matchAfterUnionWithResult = basicColl.aggregate(matchAfterUnionWith).toArray();
assert.fuzzySameMembers(matchAfterUnionWithResult, matchAfterUnionWithExpected, ["score"]);

// $addFields to basic collection (basicColl) prior to a $unionWith $vectorSearch.
let addFieldsToBasicPreUnionWith = [
    {
        $addFields: {
            id2: "$_id",
        },
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
    },
];
const addFieldsToBasicPreUnionWithExpected = [
    {"_id": 100, "localField": "cakes", "weird": false, "id2": 100},
    {"_id": 101, "localField": "cakes and kale", "weird": true, "id2": 101},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
let addFieldsToBasicPreUnionWithResult = basicColl.aggregate(addFieldsToBasicPreUnionWith).toArray();
assert.fuzzySameMembers(addFieldsToBasicPreUnionWithResult, addFieldsToBasicPreUnionWithExpected, ["score"]);

//$addFields to movie data collection (coll) prior to a $unionWith $vectorSearch.
let addFieldsToMoviesPreUnionWith = [
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
        },
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
    },
];
const addFieldsToMoviesPreUnionWithExpected = [
    {"_id": 0, "title": "It's a Mad, Mad, Mad, Mad World", "id2": 0},
    {"_id": 1, "title": "Battle for the Planet of the Apes", "id2": 1},
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
let addFieldsToMoviesPreUnionWithResult = moviesColl.aggregate(addFieldsToMoviesPreUnionWith).toArray();
assert.fuzzySameMembers(addFieldsToMoviesPreUnionWithResult, addFieldsToMoviesPreUnionWithExpected, ["score"]);

// Adding (non-meta related) fields within $unionWith.
let addFieldsWithinUnionWith = [
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
                {$project: {title: 1, score: {$meta: "vectorSearchScore"}}},
                {$addFields: {id2: "$_id"}},
            ]),
        },
    },
];
const addFieldsWithinUnionWithExpected = [
    {"_id": 0, "title": "It's a Mad, Mad, Mad, Mad World"},
    {"_id": 1, "title": "Battle for the Planet of the Apes"},
    {"_id": 6, "title": "Tarzan the Ape Man", "score": 1, "id2": 6},
    {"_id": 4, "title": "King Kong", "score": 0.8097645044326782, "id2": 4},
];
let addFieldsWithinUnionWithResult = moviesColl.aggregate(addFieldsWithinUnionWith).toArray();
assert.fuzzySameMembers(addFieldsWithinUnionWithResult, addFieldsWithinUnionWithExpected, ["score"]);

// Adding fields after $unionWith.
let addFieldsAfterUnionWith = [
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
            pipeline: getVSQuery(2).concat([{$project: {title: 1, score: {$meta: "vectorSearchScore"}}}]),
        },
    },
    {
        $addFields: {
            id2: "$_id",
        },
    },
];
const addFieldsAfterUnionWithExpected = [
    {"_id": 0, "title": "It's a Mad, Mad, Mad, Mad World", "id2": 0},
    {"_id": 1, "title": "Battle for the Planet of the Apes", "id2": 1},
    {"_id": 6, "title": "Tarzan the Ape Man", "score": 1, "id2": 6},
    {"_id": 4, "title": "King Kong", "score": 0.8097645044326782, "id2": 4},
];
let addFieldsAfterUnionWithResult = moviesColl.aggregate(addFieldsAfterUnionWith).toArray();
assert.fuzzySameMembers(addFieldsAfterUnionWithResult, addFieldsAfterUnionWithExpected, ["score"]);

// Sorting by score AFTER a unionWith.
let sortAfterUnionWith = [
    {
        $addFields: {score: 2},
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSQuery(2).concat([{$project: {_id: 1, title: 1, score: {$meta: "vectorSearchScore"}}}]),
        },
    },
    {$sort: {score: -1}},
];
const sortAfterUnionWithExpected = [
    {"_id": 100, "localField": "cakes", "weird": false, "score": 2},
    {"_id": 101, "localField": "cakes and kale", "weird": true, "score": 2},
    {"_id": 6, "title": "Tarzan the Ape Man", "score": 1},
    {"_id": 4, "title": "King Kong", "score": 0.8097645044326782},
];
let sortAfterUnionWithResult = basicColl.aggregate(sortAfterUnionWith).toArray();
assertDocArrExpectedFuzzy(sortAfterUnionWithResult, sortAfterUnionWithExpected);

// Set up to test with a $lookup stage.
const lookupCollName = "union_with_vector_search_lookup";
const lookupColl = db.getCollection(lookupCollName);
lookupColl.drop();

assert.commandWorked(lookupColl.insert({_id: 1, blob: true}));
assert.commandWorked(lookupColl.insert({_id: 2, blob: true}));

// {$lookup (non-VS)}, {$unionWith $vectorSearch}.
let lookupThenUnionWithVS = [
    {
        $lookup: {from: lookupColl.getName(), localField: "weird", foreignField: "blob", as: "result"},
    },
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2),
        },
    },
];
const lookupThenUnionWithVSExpected = [
    {"_id": 100, "localField": "cakes", "weird": false, "result": []},
    {
        "_id": 101,
        "localField": "cakes and kale",
        "weird": true,
        "result": [
            {"_id": 1, "blob": true},
            {"_id": 2, "blob": true},
        ],
    },
    {"title": "Tarzan the Ape Man", "score": 1},
    {"title": "King Kong", "score": 0.8097645044326782},
];
let lookupThenUnionWithVSResult = basicColl.aggregate(lookupThenUnionWithVS).toArray();
assert.fuzzySameMembers(lookupThenUnionWithVSResult, lookupThenUnionWithVSExpected, ["score"]);

// {$unionWith $vectorSearch}, {$lookup (non-VS)}.
let unionWithVSThenLookup = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: getVSPipeline(2).concat([{$addFields: {"weird": {$lt: ["$score", 1]}}}]),
        },
    },
    {
        $lookup: {from: lookupColl.getName(), localField: "weird", foreignField: "blob", as: "result"},
    },
];
const unionWithVSThenLookupExpected = [
    {"_id": 100, "localField": "cakes", "weird": false, "result": []},
    {
        "_id": 101,
        "localField": "cakes and kale",
        "weird": true,
        "result": [
            {"_id": 1, "blob": true},
            {"_id": 2, "blob": true},
        ],
    },
    {"title": "Tarzan the Ape Man", "score": 1, "weird": false, "result": []},
    {
        "title": "King Kong",
        "score": 0.8097645044326782,
        "weird": true,
        "result": [
            {"_id": 1, "blob": true},
            {"_id": 2, "blob": true},
        ],
    },
];
let unionWithVSThenLookupResult = basicColl.aggregate(unionWithVSThenLookup).toArray();
assert.fuzzySameMembers(unionWithVSThenLookupResult, unionWithVSThenLookupExpected, ["score"]);

// Failure Case: The query should fail when $vectorSearch is not the first stage of the
// $unionWith pipeline (because $vectorSearch must be the first stage of any pipeline
// it's in).)
let unionWithMatchVS = [
    {
        $unionWith: {
            coll: moviesCollName,
            pipeline: [{$match: {title: "Titanic"}}].concat(getVSPipeline(2)),
        },
    },
];
assert.commandFailedWithCode(db.runCommand({aggregate: basicCollName, pipeline: unionWithMatchVS, cursor: {}}), 40602);

dropSearchIndex(moviesColl, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(moviesColl, {name: getMovieVectorSearchIndexSpec().name});
dropSearchIndex(moviesCollB, {name: getMovieSearchIndexSpec().name});
