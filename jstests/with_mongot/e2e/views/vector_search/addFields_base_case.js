/**
 * This test ensures that $vectorSearch works with an $addFields view pipeline.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 *
 * TODO SERVER-106939: Run $vectorSearch with and without storedSource.
 *
 */
import {
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

const view = createMoviesViewAndIndex(datasets.MOVIES_WITH_ENRICHED_TITLE);
const vectorSearchQuery = makeMovieVectorQuery({
    queryVector: getMoviePlotEmbeddingById(6),
    limit: 5,
    indexName: datasets.MOVIES_WITH_ENRICHED_TITLE.indexName
});

// ===============================================================================
// Case 1: Basic vector search.
// ===============================================================================
validateSearchExplain(view, [vectorSearchQuery], false, enrichedTitleViewPipeline);

let expected = buildExpectedResults([6, 4, 8, 9, 10], datasets.MOVIES_WITH_ENRICHED_TITLE);
let results = view.aggregate([vectorSearchQuery]).toArray();
assertDocArrExpectedFuzzy(expected, results);

// ===============================================================================
// Case 2: Sort by enriched title.
// ===============================================================================
const sortedPipeline = [vectorSearchQuery, {$sort: {"enriched_title": 1}}];

validateSearchExplain(view, sortedPipeline, false, enrichedTitleViewPipeline);

// Expected results sorted by enriched_title.
expected = buildExpectedResults([10, 4, 6, 8, 9], datasets.MOVIES_WITH_ENRICHED_TITLE);
results = view.aggregate(sortedPipeline).toArray();
assertDocArrExpectedFuzzy(expected, results);

// ===============================================================================
// Case 3: Filter by field.
// ===============================================================================
const filterPipeline = [
    vectorSearchQuery,
    {
        "$match": {
            "enriched_title": {
                // Find all enriched titles that contain the specified value.
                "$regex": "6",
            }
        }
    }
];

validateSearchExplain(view, filterPipeline, false, enrichedTitleViewPipeline);

// Expected results filtered by regex pattern.
expected = buildExpectedResults([6], datasets.MOVIES_WITH_ENRICHED_TITLE);
results = view.aggregate(filterPipeline).toArray();
assertDocArrExpectedFuzzy(expected, results);