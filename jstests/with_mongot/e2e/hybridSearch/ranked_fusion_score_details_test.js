/*
 * Tests hybrid search $rankFusion score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e/lib/data/movies.js";

const collName = "search_rank_fusion";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));
// Index is blocking by default so that the query is only run after index has been made.
createSearchIndex(coll, getMovieSearchIndexSpec());

// Create vector search index on movie plot embeddings.
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const limit = 20;
// Multiplication factor of limit for numCandidates in $vectorSearch.
const vectorSearchOverrequestFactor = 10;

const vectorStageSpec = {
    // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
    queryVector: getMoviePlotEmbeddingById(6),
    path: "plot_embedding",
    numCandidates: limit * vectorSearchOverrequestFactor,
    index: getMovieVectorSearchIndexSpec().name,
    limit: limit,
};
const vectorStage = {
    $vectorSearch: vectorStageSpec
};

const searchStageSpec = {
    index: getMovieSearchIndexSpec().name,
    text: {query: "ape", path: ["fullplot", "title"]},
    scoreDetails: true
};

const searchStage = {
    $search: searchStageSpec
};

// Test search/vectorSearch where only search has scoreDetails.
let testQuery = [
    {
        $rankFusion: {
            input: {pipelines: {vector: [vectorStage], search: [searchStage, {$limit: limit}]}},
            scoreDetails: true,
        },
    },
    {$addFields: {details: {$meta: "scoreDetails"}, score: {$meta: "score"}}},
    {$project: {plot_embedding: 0}}
];

let results = coll.aggregate(testQuery).toArray();

function assertFieldPresent(field, containingObj) {
    assert(containingObj.hasOwnProperty(field),
           "Looked for " + tojson(field) + " in " + tojson(containingObj));
}

let searchRankSet = [];
let vectorRankSet = [];
for (const foundDoc of results) {
    assertFieldPresent("details", foundDoc);
    const details = foundDoc["details"];
    assertFieldPresent("value", details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0);
    // Description of rank fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assertFieldPresent("search", subDetails);
    assertFieldPresent("vector", subDetails);
    const searchDetails = subDetails["search"];
    assertFieldPresent("rank", searchDetails);
    // If there isn't a value, we didn't get this back from search at all.
    // if ("value" in searchDetails) {
    if (searchDetails.hasOwnProperty("value")) {
        // Make sure we didn't duplicate a rank in the search results.
        assert(!(searchDetails["rank"] in searchRankSet));
        searchRankSet.push(searchDetails["rank"]);
        assertFieldPresent("value", searchDetails);  // Output of rank calculation.
        assertFieldPresent("details",
                           searchDetails);  // Not checking description contents, just that its
                                            // present and not our placeholder value.
        assert.neq(searchDetails["details"], "Not Calculated");
        // Note we won't check the shape of the search scoreDetails beyond here.
    }

    const vectorDetails = subDetails["vector"];
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], "Not Calculated");
    assertFieldPresent("rank", vectorDetails);
    // Make sure we haven't seen this rank before.
    assert(!(vectorDetails["rank"] in vectorRankSet));
    vectorRankSet.push(vectorDetails["rank"]);
}

// Test vectorSearch/vectorSearch where neither has score details.
testQuery = [
    {
        $rankFusion: {
            input: {pipelines: {vector: [vectorStage], secondVector: [vectorStage]}},
            scoreDetails: true,
        },
    },
    {$project: {details: {$meta: "scoreDetails"}, score: {$meta: "score"}}}
];
results = coll.aggregate(testQuery).toArray();

let secondVectorRankSet = [];
vectorRankSet = [];
for (const foundDoc of results) {
    assertFieldPresent("details", foundDoc);
    const details = foundDoc["details"];
    assertFieldPresent("value", details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0);
    // Description of rank fusion. Wrapper on both secondVector / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assertFieldPresent("secondVector", subDetails);
    assertFieldPresent("vector", subDetails);
    const secondVectorDetails = subDetails["secondVector"];
    assertFieldPresent("rank", secondVectorDetails);
    // Make sure we didn't duplicate a rank in the secondVector results.
    assert(!(secondVectorDetails["rank"] in secondVectorRankSet));
    secondVectorRankSet.push(secondVectorDetails["rank"]);
    assertFieldPresent("value", secondVectorDetails);  // Output of rank calculation.
    assertFieldPresent("details",
                       secondVectorDetails);  // Not checking description contents, just that its
                                              // present and not our placeholder value.
    assert.eq(secondVectorDetails["details"], "Not Calculated");

    const vectorDetails = subDetails["vector"];
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], "Not Calculated");
    assertFieldPresent("rank", vectorDetails);
    // Make sure we haven't seen this rank before.
    assert(!(vectorDetails["rank"] in vectorRankSet));
    vectorRankSet.push(vectorDetails["rank"]);
}

// Test search/vectorSearch where search scoreDetails is off.
const searchStageSpecNoDetails = {
    index: getMovieSearchIndexSpec().name,
    text: {query: "ape", path: ["fullplot", "title"]},
    scoreDetails: false
};

const searchStageNoDetails = {
    $search: searchStageSpecNoDetails
};

testQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {vector: [vectorStage], search: [searchStageNoDetails, {$limit: limit}]}
            },
            scoreDetails: true,
        },
    },
    {$addFields: {details: {$meta: "scoreDetails"}, score: {$meta: "score"}}},
    {$project: {plot_embedding: 0}}
];

results = coll.aggregate(testQuery).toArray();
searchRankSet = [];
vectorRankSet = [];
for (const foundDoc of results) {
    assertFieldPresent("details", foundDoc);
    const details = foundDoc["details"];
    assertFieldPresent("value", details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0);
    // Description of rank fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assertFieldPresent("search", subDetails);
    assertFieldPresent("vector", subDetails);
    const searchDetails = subDetails["search"];
    assertFieldPresent("rank", searchDetails);
    // If there isn't a value, we didn't get this back from search at all.
    // if ("value" in searchDetails) {
    if (searchDetails.hasOwnProperty("value")) {
        // Make sure we didn't duplicate a rank in the search results.
        assert(!(searchDetails["rank"] in searchRankSet));
        searchRankSet.push(searchDetails["rank"]);
        assertFieldPresent("value", searchDetails);  // Output of rank calculation.
        assertFieldPresent("details", searchDetails);
        assert.eq(searchDetails["details"], "Not Calculated");
        // Note we won't check the shape of the search scoreDetails beyond here.
    }

    const vectorDetails = subDetails["vector"];
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], "Not Calculated");
    assertFieldPresent("rank", vectorDetails);
    // Make sure we haven't seen this rank before.
    assert(!(vectorDetails["rank"] in vectorRankSet));
    vectorRankSet.push(vectorDetails["rank"]);
}

// TODO SERVER-93218 Test scoreDetails with nested rankFusion.
dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
