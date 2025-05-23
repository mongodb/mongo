/**
 * This test ensures we support running $vectorSearch on views.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const collName = "search_views";
const coll = testDb.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

const viewName = "vector_view";
const viewPipeline = [{"$addFields": {aa_type: {$ifNull: ['$aa_type', 'foo']}}}];
assert.commandWorked(testDb.createView(viewName, collName, viewPipeline));
const vectorView = testDb[viewName];

// Get vector search index specification.
const vectorIndexSpec = getMovieVectorSearchIndexSpec();

const indexConfig = {
    coll: vectorView,
    definition: vectorIndexSpec
};

const vectorSearchTestCases = () => {
    const pipeline = [{
        "$vectorSearch": {
            "queryVector": getMoviePlotEmbeddingById(6),
            "path": "plot_embedding",
            "numCandidates": 100,
            "limit": 5,
            "index": vectorIndexSpec.name,
        }
    }];

    validateSearchExplain(vectorView, pipeline, false, viewPipeline);
};

createSearchIndexesAndExecuteTests(indexConfig, vectorSearchTestCases, false);
