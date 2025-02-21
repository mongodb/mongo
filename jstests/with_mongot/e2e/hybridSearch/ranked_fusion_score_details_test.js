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

const scoreDetailsDescription =
    "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 " +
    "+ rank))) across input pipelines from which this document is output, from:";

// Test search/vectorSearch where only search has scoreDetails.
let testQuery = [
    {
        $rankFusion: {
            input: {pipelines: {vector: [vectorStage], search: [searchStage, {$limit: limit}]}},
            combination: {weights: {search: 2}},
            scoreDetails: true,
        },
    },
    {$addFields: {details: {$meta: "scoreDetails"}}},
    {$project: {plot_embedding: 0}}
];

let results = coll.aggregate(testQuery).toArray();

function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

for (const foundDoc of results) {
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0, details);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreDetailsDescription);

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of rank fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const searchDetails = subDetails[0];
    assertFieldPresent("inputPipelineName", searchDetails);
    assert.eq(searchDetails["inputPipelineName"], "search");
    assertFieldPresent("rank", searchDetails);
    assertFieldPresent("weight", searchDetails);
    assert.eq(searchDetails["weight"], 2);
    // If there isn't a value, we didn't get this back from search at all.
    if (searchDetails.hasOwnProperty("value")) {
        assertFieldPresent("value", searchDetails);  // Output of rank calculation.
        assertFieldPresent("details",
                           searchDetails);  // Not checking description contents, just that its
                                            // present and not our placeholder value.
        assert.neq(searchDetails["details"], []);
        // Note we won't check the shape of the search scoreDetails beyond here.
    }

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
    assertFieldPresent("rank", vectorDetails);
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 1);
}

// Test vectorSearch/vectorSearch where neither has score details.
testQuery = [
    {
        $rankFusion: {
            input: {pipelines: {vector: [vectorStage], secondVector: [vectorStage]}},
            combination: {weights: {vector: 0.5, secondVector: 2.8}},
            scoreDetails: true,
        },
    },
    {$project: {details: {$meta: "scoreDetails"}}}
];
results = coll.aggregate(testQuery).toArray();

for (const foundDoc of results) {
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // The output of the rank calculation.
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreDetailsDescription);

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of rank fusion. Wrapper on both secondVector / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const secondVectorDetails = subDetails[0];
    assertFieldPresent("inputPipelineName", secondVectorDetails);
    assert.eq(secondVectorDetails["inputPipelineName"], "secondVector");
    assertFieldPresent("rank", secondVectorDetails);
    assertFieldPresent("weight", secondVectorDetails);
    assert.eq(secondVectorDetails["weight"], 2.8);
    assertFieldPresent("value", secondVectorDetails);  // Original 'score' AKA vectorSearchScore.
    assertFieldPresent("details",
                       secondVectorDetails);  // Not checking description contents, just that its
                                              // present and not our placeholder value.
    assert.eq(secondVectorDetails["details"], []);

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
    assertFieldPresent("value", vectorDetails);  // Original 'score' AKA vectorSearchScore.
    assertFieldPresent("rank", vectorDetails);
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 0.5);
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
    {$addFields: {details: {$meta: "scoreDetails"}}},
    {$project: {plot_embedding: 0}}
];

results = coll.aggregate(testQuery).toArray();
for (const foundDoc of results) {
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreDetailsDescription);

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of rank fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const searchDetails = subDetails[0];
    assertFieldPresent("inputPipelineName", searchDetails);
    assert.eq(searchDetails["inputPipelineName"], "search");
    assertFieldPresent("rank", searchDetails);
    assertFieldPresent("weight", searchDetails);
    assert.eq(searchDetails["weight"], 1);
    // If there isn't a value, we didn't get this back from search at all.
    if (searchDetails.hasOwnProperty("value")) {
        assertFieldPresent("value", searchDetails);  // Output of rank calculation.
        assertFieldPresent("details", searchDetails);
        assert.eq(searchDetails["details"], []);
        // Note we won't check the shape of the search scoreDetails beyond here.
    }

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
    assertFieldPresent("rank", vectorDetails);
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 1);
}

// TODO SERVER-93218 Test scoreDetails with nested rankFusion.
dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
