/**
 * Testing that the explain output for $vectorSearch includes a dummy value for "queryVector"
 * instead of the embeddings.
 */
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e/lib/data/movies.js";

// Load Collection
const coll = db.sort_by_vector_search_score;
coll.drop();
const allSeedData = getMovieData();
assert.commandWorked(coll.insertMany(allSeedData));

// Create vector search index on movie plot embeddings.
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

// Call explain and assert "queryVector" embeddings values not included.
function testExplainVerbosity(verbosity) {
    // Including only required fields for $vectorSearch pipeline stage
    const limit = 20;
    const vectorSearchOverrequestFactor =
        10;  // Multiplication factor of k for numCandidates in $vectorSearch.

    let query = [
        {
            $vectorSearch: {
                queryVector: getMoviePlotEmbeddingById(6),  // embedding for 'Tarzan the Ape Man'
                path: "plot_embedding",
                numCandidates: limit * vectorSearchOverrequestFactor,
                index: getMovieVectorSearchIndexSpec().name,
                limit: limit,
            }
        },
    ];

    const result = coll.explain(verbosity).aggregate(query);
    const substitute = "redacted";
    if (!result.hasOwnProperty("shards")) {  // single node test suite
        const vectorSearchStage = getAggPlanStage(result, "$vectorSearch");
        assert(vectorSearchStage, vectorSearchStage);
        assert(vectorSearchStage["$vectorSearch"].hasOwnProperty("queryVector"));
        const vectorSearchWithQueryVector = vectorSearchStage["$vectorSearch"]["queryVector"];
        // Confirm associated value with queryVector matches expected
        assert.eq(vectorSearchWithQueryVector, substitute);
    } else {  // sharded cluster test suite
        const numShards = Object.keys(result["shards"]).length;
        // Confirm associated value with queryVector for each shard matches expected
        for (const [_, value] of Object.entries(numShards)) {
            assert(value.hasOwnProperty("stages"));
            assert(value["stages"][0].hasOwnProperty("$vectorSearch"));
            assert(value["stages"][0]["$vectorSearch"].hasOwnProperty("queryVector"));
            assert.eq(value["stages"][0]["$vectorSearch"]["queryVector"], substitute);
        }
    }
}

testExplainVerbosity("queryPlanner");  // Currently the only option.
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
