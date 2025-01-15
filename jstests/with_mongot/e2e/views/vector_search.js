/**
 * This test ensures we support running $vectorSearch on views.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e/lib/data/movies.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const collName = "search_views";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

let viewName = "vector_view";
let viewPipeline = [{"$addFields": {aa_type: {$ifNull: ['$aa_type', 'foo']}}}];
assert.commandWorked(db.createView(viewName, 'search_views', viewPipeline));
let addFieldsView = db[viewName];

// Create vector search index on movie plot embeddings.
createSearchIndex(addFieldsView, getMovieVectorSearchIndexSpec());

// Query for similar documents.
let pipeline = [{
    "$vectorSearch": {
        "queryVector": getMoviePlotEmbeddingById(6),
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
dropSearchIndex(addFieldsView, {name: getMovieVectorSearchIndexSpec().name});
