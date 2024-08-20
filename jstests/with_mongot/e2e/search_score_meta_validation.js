/**
 * Tests the validation of using "searchScore" and "vectorSearchScore" metadata fields.
 * @tags: [featureFlagSearchHybridScoringPrerequisites]
 */
import {
    getMovieData,
    getPlotEmbeddingById,
    getVectorSearchIndexSpec,
    makeVectorQuery
} from "jstests/with_mongot/e2e/lib/hybrid_scoring_data.js";

const coll = db.search_score_meta_validation;
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));
assert.commandWorked(coll.createSearchIndex(getVectorSearchIndexSpec()));

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
//             pipeline: [makeVectorQuery({
//                 queryVector: getPlotEmbeddingById(8),
//                 limit: 10,
//             })]
//         }
//     },
//     {$sort: {score: {$meta: "vectorSearchScore"}}}
// ]));
