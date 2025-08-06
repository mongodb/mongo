/**
 * Tests the validation of using "searchScore" and "vectorSearchScore" metadata fields.
 * @tags: [featureFlagRankFusionFull, requires_fcv_81]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec,
    makeMovieVectorQuery
} from "jstests/with_mongot/e2e_lib/data/movies.js";

const kUnavailableMetadataErrCode = 40218;

const coll = db.search_score_meta_validation;
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

function testInvalidDereference(metaType) {
    assert.throwsWithCode(() => coll.aggregate([
        {$setWindowFields: {sortBy: {score: {$meta: metaType}}, output: {rank: {$rank: {}}}}},
    ]),
                          kUnavailableMetadataErrCode);
    assert.throwsWithCode(() => coll.aggregate([{$sort: {score: {$meta: metaType}}}]),
                          kUnavailableMetadataErrCode);
    assert.throwsWithCode(() => coll.aggregate([{$set: {score: {$meta: metaType}}}]),
                          kUnavailableMetadataErrCode);
}

testInvalidDereference("searchScore");
testInvalidDereference("vectorSearchScore");

assert.throwsWithCode(() => coll.aggregate([
    {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [makeMovieVectorQuery({
                queryVector: getMoviePlotEmbeddingById(8),
                limit: 10,
            })]
        }
    },
    {$sort: {score: {$meta: "vectorSearchScore"}}}
]),
                      kUnavailableMetadataErrCode);
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
