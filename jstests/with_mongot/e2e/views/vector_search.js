/**
 * This test ensures we support running $vectorSearch on views
 *
 * @tags: [
 * requires_mongot_1_40
 * ]
 */
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";
import {
    buildExpectedResults,
    getMovieData,
    getPlotEmbeddingById
} from "jstests/with_mongot/e2e/lib/hybrid_scoring_data.js";
import {assertDocArrExpectedFuzzy} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

const collName = "search_views";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

let viewName = "vector_view";
let viewPipeline = [{"$addFields": {aa_type: {$ifNull: ['$aa_type', 'foo']}}}];
assert.commandWorked(db.createView(viewName, 'search_views', viewPipeline));
let addFieldsView = db[viewName];

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
addFieldsView.createSearchIndex(vectorIndex);

// Query for similar documents.
let pipeline = [{
    "$vectorSearch": {
        "queryVector": getPlotEmbeddingById(6),
        "path": "plot_embedding",
        "numCandidates": 100,
        "limit": 5,
        "index": "moviesPlotIndex",
    }
}];
const explainResults = addFieldsView.explain().aggregate(pipeline)["stages"];
// Assert $addFields is the last stage of the pipeline, which ensures that the view pipeline follows
// the vectorSearch related stages (eg $_internalSearchMongotRemote, $_internalSearchIdLookup,
// $vectorSearch)
assertViewAppliedCorrectly(explainResults, pipeline, viewPipeline);
