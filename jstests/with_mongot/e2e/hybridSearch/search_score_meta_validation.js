/**
 * Tests the validation of using "searchScore" and "vectorSearchScore" metadata fields.
 * @tags: [featureFlagSearchHybridScoringPrerequisites]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec,
    makeMovieVectorQuery
} from "jstests/with_mongot/e2e/lib/data/movies.js";

const coll = db.search_score_meta_validation;
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

function testInvalidDereference(metaType) {
    // TODO SERVER-93521 Should not be able to use 'searchScore' without a $search pipeline. These
    // pipelines should one day return an error, since the behavior is essentially undefined.
    assert.doesNotThrow(() => coll.aggregate([
        {$setWindowFields: {sortBy: {score: {$meta: metaType}}, output: {rank: {$rank: {}}}}},
    ]));
    assert.doesNotThrow(() => coll.aggregate([{$sort: {score: {$meta: metaType}}}]));
    assert.doesNotThrow(() => coll.aggregate([{$set: {score: {$meta: metaType}}}]));
}

testInvalidDereference("searchScore");
testInvalidDereference("vectorSearchScore");

// TODO SERVER-93521 This one should error since half of the documents don't have a
// vectorSearchScore.
// assert.doesNotThrow(() => coll.aggregate([
//     {
//         $unionWith: {
//             coll: coll.getName(),
//             pipeline: [makeMovieVectorQuery({
//                 queryVector: getMoviePlotEmbeddingById(8),
//                 limit: 10,
//             })]
//         }
//     },
//     {$sort: {score: {$meta: "vectorSearchScore"}}}
// ]));
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
