/**
 * This test ensures that $vectorSearch works with a $match view pipeline.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 *
 * TODO SERVER-106939: Run $vectorSearch with and without storedSource.
 *
 */
import {
    actionMoviesViewPipeline,
    createMoviesViewAndIndex,
    getMoviePlotEmbeddingById,
    makeMovieVectorQuery
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
    validateSearchExplain
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const view = createMoviesViewAndIndex(datasets.ACTION_MOVIES);
const vectorSearchQuery = makeMovieVectorQuery({
    queryVector: getMoviePlotEmbeddingById(6),
    limit: 5,
    indexName: datasets.ACTION_MOVIES.indexName
});

// ===============================================================================
// Case 1: Basic vector search.
// ===============================================================================
validateSearchExplain(view, [vectorSearchQuery], false, actionMoviesViewPipeline);

let expected = buildExpectedResults([6, 9, 10, 12, 13], datasets.MOVIES);
let results = view.aggregate([vectorSearchQuery]).toArray();
assertDocArrExpectedFuzzy(expected, results);

// ===============================================================================
// Case 2: Sort by title.
// ===============================================================================
const sortedPipeline = [vectorSearchQuery, {$sort: {"title": 1}}];

validateSearchExplain(view, sortedPipeline, false, actionMoviesViewPipeline);

// Expected results sorted by title.
expected = buildExpectedResults([10, 12, 13, 9, 6], datasets.MOVIES);
results = view.aggregate(sortedPipeline).toArray();
assertDocArrExpectedFuzzy(expected, results);

// ===============================================================================
// Case 3: Filter by additional genre.
// ===============================================================================
const filterPipeline = [vectorSearchQuery, {$match: {$expr: {$in: ["Sci-Fi", "$genres"]}}}];

validateSearchExplain(view, filterPipeline, false, actionMoviesViewPipeline);

// Expected results also filtered by genre "Sci-Fi".
expected = buildExpectedResults([9, 10], datasets.MOVIES);
results = view.aggregate(filterPipeline).toArray();
assertDocArrExpectedFuzzy(expected, results);