/*
 * Tests hybrid search with the rank fusion using the $rankFusion stage.
 * @tags: [ featureFlagSearchHybridScoringPrerequisites ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    buildExpectedResults,
    getMovieData,
    getPlotEmbeddingById
} from "jstests/with_mongot/e2e/lib/hybrid_scoring_data.js";
import {assertDocArrExpectedFuzzy} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

const collName = "search_rank_fusion";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

createSearchIndex(coll, {name: "search_movie", definition: {"mappings": {"dynamic": true}}});

// Create vector search index on movie plot embeddings.
const vectorIndex = {
    name: "vector_search_movie",
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
createSearchIndex(coll, vectorIndex);

const limit = 20;
// Multiplication factor of limit for numCandidates in $vectorSearch.
const vectorSearchOverrequestFactor = 10;

let testQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    vector: [{
                        $vectorSearch: {
                            // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
                            queryVector: getPlotEmbeddingById(6),
                            path: "plot_embedding",
                            numCandidates: limit * vectorSearchOverrequestFactor,
                            index: "vector_search_movie",
                            limit: limit,
                        }
                    }],
                    search: [
                        {
                            $search: {
                                index: "search_movie",
                                text: {query: "ape", path: ["fullplot", "title"]},
                            }
                        },
                        {$limit: limit}
                    ]
                }
            }
        },
    },
    {$limit: limit}
];

let results = coll.aggregate(testQuery).toArray();

let expectedResultIds = [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds), results);

dropSearchIndex(coll, {name: "search_movie"});
dropSearchIndex(coll, {name: "vector_search_movie"});
