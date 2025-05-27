/**
 * This file uses $unionWith to join two $vectorSearch aggregations on a combination of views and
 * collections. The purpose of which is to test running $vectorSearch on mongot-indexed views.
 * Each of the three test cases inspects explain output for execution pipeline correctness.
 * 1. Outer view and inner view.
 * 2. Outer view and inner collection.
 * 3. Outer collection and inner view.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {
    actionMoviesViewPipeline,
    createMoviesCollAndIndex,
    createMoviesViewAndIndex,
    enrichedTitleViewPipeline,
    getMoviePlotEmbeddingById,
    makeMovieVectorQuery
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertUnionWithSearchSubPipelineAppliedViews
} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
    validateSearchExplain
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// There is currently only one dataset for $vectorSearch, so we must use it for the collection and
// both views in this test.
const moviesWithEnrichedTitle = createMoviesViewAndIndex(datasets.MOVIES_WITH_ENRICHED_TITLE);
const actionMovies = createMoviesViewAndIndex(datasets.ACTION_MOVIES);
const moviesColl = createMoviesCollAndIndex();
const moviesWithEnrichedTitleQuery = makeMovieVectorQuery({
    queryVector: getMoviePlotEmbeddingById(6),
    limit: 5,
    indexName: datasets.MOVIES_WITH_ENRICHED_TITLE.indexName
});
const actionMoviesQuery = makeMovieVectorQuery({
    queryVector: getMoviePlotEmbeddingById(11),
    limit: 2,
    indexName: datasets.ACTION_MOVIES.indexName
});
const moviesCollQuery = makeMovieVectorQuery(
    {queryVector: getMoviePlotEmbeddingById(11), limit: 3, indexName: datasets.MOVIES.indexName});

// ===============================================================================
// Case 1: $unionWith on outer and inner views.
// ===============================================================================
let pipeline = [
    moviesWithEnrichedTitleQuery,
    {$unionWith: {coll: actionMovies.getName(), pipeline: [actionMoviesQuery]}}
];

validateSearchExplain(
    moviesWithEnrichedTitle, pipeline, false, enrichedTitleViewPipeline, (explain) => {
        assertUnionWithSearchSubPipelineAppliedViews(
            explain, moviesColl, actionMovies, actionMoviesViewPipeline);
    });

let outerExpected = buildExpectedResults([6, 4, 8, 9, 10], datasets.MOVIES_WITH_ENRICHED_TITLE);
let innerExpected = buildExpectedResults([11, 5], datasets.MOVIES);

let results = moviesWithEnrichedTitle.aggregate(pipeline).toArray();
assertDocArrExpectedFuzzy([...outerExpected, ...innerExpected], results);
// ===============================================================================
// Case 2: $unionWith on an outer view and inner collection.
// ===============================================================================
pipeline = [
    moviesWithEnrichedTitleQuery,
    {$unionWith: {coll: moviesColl.getName(), pipeline: [moviesCollQuery]}}
];

validateSearchExplain(moviesWithEnrichedTitle, pipeline, false, enrichedTitleViewPipeline);

outerExpected = buildExpectedResults([6, 4, 8, 9, 10], datasets.MOVIES_WITH_ENRICHED_TITLE);
innerExpected = buildExpectedResults([11, 14, 5], datasets.MOVIES);

results = moviesWithEnrichedTitle.aggregate(pipeline).toArray();
assertDocArrExpectedFuzzy([...outerExpected, ...innerExpected], results);

// ===============================================================================
// Case 3: $unionWith on an outer collection and inner view.
// ===============================================================================
pipeline = [
    moviesCollQuery,
    {
        $unionWith:
            {coll: moviesWithEnrichedTitle.getName(), pipeline: [moviesWithEnrichedTitleQuery]}
    }
];

validateSearchExplain(moviesColl, pipeline, false, null, (explain) => {
    assertUnionWithSearchSubPipelineAppliedViews(
        explain, moviesColl, moviesWithEnrichedTitle, enrichedTitleViewPipeline);
});

outerExpected = buildExpectedResults([11, 14, 5], datasets.MOVIES);
innerExpected = buildExpectedResults([6, 4, 8, 9, 10], datasets.MOVIES_WITH_ENRICHED_TITLE);

results = moviesColl.aggregate(pipeline).toArray();
assertDocArrExpectedFuzzy([...outerExpected, ...innerExpected], results);
