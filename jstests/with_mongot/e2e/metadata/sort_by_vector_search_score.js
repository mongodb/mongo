/**
 * Tests using "vectorSearchScore" (and its equivalent alias metadata field "score") in a sort
 * expression. This isn't expected to be very common, but one anticipated use case is to compute
 * rank or other window fields, where a sort expression is required.
 * @tags: [featureFlagRankFusionFull, requires_fcv_81]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec,
    makeMovieVectorExactQuery,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {waitUntilDocIsVisibleByQuery} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// Main testing function that runs all sub-tests.
// Input parameter is the name of the meta field that should be sorted on
// (i.e. "vectorSearchScore" or "score")
function runTest(metadataSortFieldName) {
    const coll = db.sort_by_vector_search_score;
    coll.drop();
    const allSeedData = getMovieData();
    assert.commandWorked(coll.insertMany(allSeedData));

    // Create vector search index on movie plot embeddings.
    createSearchIndex(coll, getMovieVectorSearchIndexSpec());

    // Our main use case of interest: using the score to compute a rank, with no 'partitionBy'.
    const testRankingPipeline = [
        // Get the embedding for 'Beauty and the Beast', which has _id = 14.
        makeMovieVectorExactQuery({queryVector: getMoviePlotEmbeddingById(14), limit: 10}),
        {
            $setWindowFields: {sortBy: {score: {$meta: metadataSortFieldName}}, output: {rank: {$rank: {}}}},
        },
        {$sort: {score: {$meta: metadataSortFieldName}, _id: 1}},
        {$project: {rank: 1, score: {$meta: metadataSortFieldName}, _id: 1}},
    ];
    {
        const results = coll.aggregate(testRankingPipeline).toArray();

        // So far these are all unique movies with unique embeddings, so we should see unique ranks:
        // 1 through 10.
        assert.eq(results.length, 10);
        assert.eq(results[0]._id, 14, results[0]); // The one we queried for should be first place.
        for (let i = 0; i < results.length; ++i) {
            // Adjusting for off-by-one, since 'i' is 0-based indexing and rank is 1-based.
            assert.eq(results[i].rank, i + 1, results[i]);
        }
    }

    {
        // Test with a 'partitionBy' argument. We segment the even _ids and the odd _ids, which
        // should create two streams of ranks of approximately equal size.
        const results = coll
            .aggregate([
                makeMovieVectorExactQuery({queryVector: getMoviePlotEmbeddingById(14), limit: 20}),
                {
                    $setWindowFields: {
                        sortBy: {score: {$meta: metadataSortFieldName}},
                        partitionBy: {$mod: ["$_id", 2]},
                        output: {rank: {$rank: {}}},
                    },
                },
                {$sort: {score: {$meta: metadataSortFieldName}, _id: 1}},
                {$project: {rank: 1, score: 1, _id: 1}},
            ])
            .toArray();

        assert.eq(
            results.length,
            allSeedData.length - 1,
            "A higher limit, we should see all results now except (a) the duplicate result which we filtered out and (b) the one which had a null query vector",
        );

        // This should still be the top result.
        assert.eq(results[0]._id, 14, results[0]);
        assert.eq(results[0].rank, 1, results[0]);
        // We should have partitioned to assign ranks, meaning the last place result should not have
        // rank 'results.length' (as it would otherwise assuming no ties).
        assert.lt(results[results.length - 1].rank, results.length, results[results.length - 1]);
    }

    {
        // Now insert a duplicate record of 'Beauty and the Beast' - we should see two records with
        // rank 1, the rest should be in order now starting at rank 3.
        assert.commandWorked(
            coll.insertOne(coll.aggregate([{$match: {_id: 14}}, {$set: {_id: {$const: "duplicate"}}}]).next()),
        );
        waitUntilDocIsVisibleByQuery({
            docId: "duplicate",
            coll: coll,
            queryPipeline: [makeMovieVectorExactQuery({queryVector: getMoviePlotEmbeddingById(14), limit: 10})],
        });

        const results = coll.aggregate(testRankingPipeline).toArray();
        assert.eq(results.length, 10, "still specified a limit of 10");
        // We broke the score tie with a sort on _id. Strings come after numbers.
        assert.eq(results[0]._id, 14, results[0]);
        assert.eq(results[1]._id, "duplicate", results[1]);
        assert.eq(results[0].rank, 1, results[0]);
        assert.eq(results[1].rank, 1, results[1]);
        for (let i = 2; i < results.length; ++i) {
            // Adjusting for off-by-one, since 'i' is 0-based indexing and rank is 1-based.
            assert.eq(results[i].rank, i + 1);
        }
    }

    dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
}

runTest("vectorSearchScore");
runTest("score");
