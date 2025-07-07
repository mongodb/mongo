/**
 * This file tests $vectorSearch with a nested view pipeline.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 *
 * TODO SERVER-106939: Run $vectorSearch with and without storedSource.
 *
 */
import {
    actionMoviesViewPipeline,
    createMoviesViewAndIndex,
    enrichedTitleViewPipeline,
    getMoviePlotEmbeddingById,
    makeMovieVectorQuery
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
    validateSearchExplain
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// Create the action movies view on top of the enriched title view.
const actionMoviesWithEnrichedTitle =
    createMoviesViewAndIndex(datasets.ACTION_MOVIES_WITH_ENRICHED_TITLE);
const vectorQuery = makeMovieVectorQuery({
    queryVector: getMoviePlotEmbeddingById(6),
    limit: 5,
    indexName: datasets.ACTION_MOVIES_WITH_ENRICHED_TITLE.indexName
});

// Combine the two view pipelines for validation.
const combinedViewPipeline = enrichedTitleViewPipeline.concat(actionMoviesViewPipeline);

validateSearchExplain(actionMoviesWithEnrichedTitle, [vectorQuery], false, combinedViewPipeline);

const expected = buildExpectedResults([6, 9, 10, 12, 13], datasets.MOVIES_WITH_ENRICHED_TITLE);
const results = actionMoviesWithEnrichedTitle.aggregate([vectorQuery]).toArray();
assertDocArrExpectedFuzzy(expected, results);
