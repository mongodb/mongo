/**
 * This test verifies that when a $vectorSearch stage specifies an index that does not exist on the
 * top-level aggregation namespace or only exists on the underlying collection, the query returns no
 * results.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 *
 * TODO SERVER-106939: Run $vectorSearch with and without storedSource.
 *
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    createMoviesCollAndIndex,
    createMoviesView,
    getMoviePlotEmbeddingById,
    makeMovieVectorQuery
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    datasets,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const moviesWithEnrichedTitle = createMoviesView(datasets.MOVIES_WITH_ENRICHED_TITLE);
createMoviesCollAndIndex();

const tvShowColl = db.getSiblingDB("vector_search_shared_db").tvShowColl;
tvShowColl.drop();
tvShowColl.insertOne({title: "Breaking Bad"});

const basicQueryResult = (indexName) => {
    return moviesWithEnrichedTitle
        .aggregate(makeMovieVectorQuery(
            {queryVector: getMoviePlotEmbeddingById(11), limit: 3, indexName: indexName}))
        .toArray();
};

const unionWithQueryResult = (indexName) => {
    return tvShowColl
        .aggregate([
            {$project: {_id: 0}},
            {
                $unionWith: {
                    coll: moviesWithEnrichedTitle.getName(),
                    pipeline: [makeMovieVectorQuery({
                        queryVector: getMoviePlotEmbeddingById(11),
                        limit: 3,
                        indexName: indexName
                    })]
                }
            }
        ])
        .toArray();
};

const indexNames = [datasets.MOVIES.indexName, "thisIsNotAnIndex"];

for (const indexName of indexNames) {
    jsTestLog(`Testing index: ${indexName}`);
    assertArrayEq({actual: basicQueryResult(indexName), expected: []});
    assertArrayEq({actual: unionWithQueryResult(indexName), expected: [{title: "Breaking Bad"}]});
}
