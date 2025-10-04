/**
 * This file tests nested $unionWith pipelines involving $vectorSearch operations across views. The
 * purpose is to verify that the nested unions and searches return the correct results across all
 * views.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 *
 * TODO SERVER-106939: Run $vectorSearch with and without storedSource.
 *
 */
import {
    actionMoviesViewPipeline,
    createMoviesCollAndVectorIndex,
    createMoviesViewAndIndex,
    enrichedTitleViewPipeline,
    getMoviePlotEmbeddingById,
    makeMovieVectorExactQuery,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {assertUnionWithSearchSubPipelineAppliedViews} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
    validateSearchExplain,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const moviesWithEnrichedTitle = createMoviesViewAndIndex(datasets.MOVIES_WITH_ENRICHED_TITLE);
const actionMovies = createMoviesViewAndIndex(datasets.ACTION_MOVIES);
const moviesWithEnrichedTitleQuery = makeMovieVectorExactQuery({
    queryVector: getMoviePlotEmbeddingById(6),
    limit: 5,
    indexName: datasets.MOVIES_WITH_ENRICHED_TITLE.indexName,
});
const actionMoviesQuery = makeMovieVectorExactQuery({
    queryVector: getMoviePlotEmbeddingById(11),
    limit: 2,
    indexName: datasets.ACTION_MOVIES.indexName,
});
const moviesColl = createMoviesCollAndVectorIndex();

const pipeline = [
    // Match the top level view upon a few documents to ensure that the subsequent $unionWith stages
    // won't output repeated documents.
    {$match: {_id: {$in: [0, 1, 2, 3]}}},
    {
        $unionWith: {
            coll: actionMovies.getName(),
            pipeline: [
                actionMoviesQuery,
                {
                    $unionWith: {
                        coll: moviesWithEnrichedTitle.getName(),
                        pipeline: [moviesWithEnrichedTitleQuery],
                    },
                },
            ],
        },
    },
];

validateSearchExplain(moviesWithEnrichedTitle, pipeline, false, enrichedTitleViewPipeline, (explain) => {
    assertUnionWithSearchSubPipelineAppliedViews(explain, moviesColl, actionMovies.getName(), actionMoviesViewPipeline);
});

// Gather the expected results for all parts of the pipeline.
const topLevelExpected = buildExpectedResults([0, 1, 2, 3], datasets.MOVIES_WITH_ENRICHED_TITLE);
const outerExpected = buildExpectedResults([11, 5], datasets.MOVIES);
const innerExpected = buildExpectedResults([6, 4, 8, 9, 10], datasets.MOVIES_WITH_ENRICHED_TITLE);

const results = moviesWithEnrichedTitle.aggregate(pipeline).toArray();
assertDocArrExpectedFuzzy([...topLevelExpected, ...outerExpected, ...innerExpected], results);
