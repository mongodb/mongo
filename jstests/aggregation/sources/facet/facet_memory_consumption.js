/**
 * Test that the $facet stage fails cleanly without consuming too much memory if the size of the
 * facet's output document is large.
 *
 * This test was designed to reproduce SERVER-40317.
 */
(function() {
"use strict";

const collName = "facet_memory_consumption";
const coll = db[collName];
const kFacetOutputTooLargeCode = 4031700;
coll.drop();

// A document that is slightly less than 1MB.
const doc = {
    str: "x".repeat(1024 * 1024 - 100)
};

// Insert it into the collection twice.
assert.commandWorked(coll.insert(doc));
assert.commandWorked(coll.insert(doc));

// Creates a pipeline that chains Cartesian product pipelines to create a pipeline returning
// 2^exponent documents (assuming that there 2 documents in the 'collName' collection).
function cartesianProductPipeline(exponent) {
    let productPipeline = [];
    for (let i = 0; i < exponent - 1; ++i) {
        productPipeline = productPipeline.concat([
            {$lookup: {from: collName, pipeline: [{$match: {}}], as: "join"}},
            {$unwind: "$join"},
            {$project: {str: 1}},
        ]);
    }
    return productPipeline;
}

(function succeedsWhenWithinMemoryLimit() {
    // This pipeline uses $facet to return one document that is just slightly less than the 16MB,
    // which is within the document size limit.
    const result = coll.aggregate([{$facet: {product: cartesianProductPipeline(4)}}]).toArray();
    const resultSize = Object.bsonsize(result);

    // As a sanity check, make sure that the resulting document is somewhere around 16MB in size.
    assert.gt(resultSize, 15 * 1024 * 1024, result);
    assert.lt(resultSize, 16 * 1024 * 1024, result);
}());

(function failsWhenResultDocumentExeedsMaxBSONSize() {
    // This pipeline uses $facet to create a document that is larger than the 16MB max document
    // size.
    const result = assert.throws(
        () => coll.aggregate([{$facet: {product: cartesianProductPipeline(6)}}]).toArray());
    assert.eq(result.code, ErrorCodes.BSONObjectTooLarge);
}());

(function succeedsWhenIntermediateDocumentExceedsMaxBSONSizeWithUnwind() {
    // This pipeline uses $facet to create an intermediate document that is larger than the 16MB
    // max document size but smaller than the 100MB allowed for an intermediate document. The
    // $unwind stage breaks the large document into a bunch of small documents, which is legal.
    const result =
        coll.aggregate([{$facet: {product: cartesianProductPipeline(6)}}, {$unwind: "$product"}])
            .toArray();
    assert.eq(64, result.length, result);
}());

(function failsWhenFacetOutputDocumentTooLarge() {
    // This pipeline uses $facet to create a document that is larger than the 100MB maximum size for
    // an intermediate document. Even with the $unwind stage, the pipeline should fail, this time
    // with error code 31034.
    const result = assert.throws(
        () => coll.aggregate(
                      [{$facet: {product: cartesianProductPipeline(10)}}, {$unwind: "$product"}])
                  .toArray());
    assert.eq(result.code, kFacetOutputTooLargeCode);
}());
}());
