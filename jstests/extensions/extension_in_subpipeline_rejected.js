/**
 * Tests that using an extension stage in a sub-pipeline is rejected, for now.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.extension_in_subpipeline_rejected;
const other = db.extension_in_subpipeline_rejected_other;
coll.drop();
other.drop();

assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(other.insert({_id: 1}));

const kNotAllowedInLookupErrorCode = 51047;
const kNotAllowedInUnionWithErrorCode = 31441;
const kNotAllowedInFacetErrorCode = 40600;

/*
TODO SERVER-117179 Enable this test.
// Test that a $lookup pipeline can reject an extension stage.
{
    const lookupPipeline = [{$lookup: {from: other.getName(), as: "joined", pipeline: [{$testFoo: {}}]}}];
    assertErrorCode(
        coll,
        lookupPipeline,
        kNotAllowedInLookupErrorCode,
        "Using $lookup with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$lookup: {from: other.getName(), pipeline: lookupPipeline, as: "joined"}}],
        kNotAllowedInLookupErrorCode,
        "Using $lookup with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$unionWith: {coll: other.getName(), pipeline: lookupPipeline}}],
        kNotAllowedInLookupErrorCode,
        "Using $lookup with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$facet: {facetPipe: lookupPipeline}}],
        kNotAllowedInLookupErrorCode,
        "Using $lookup with $testFoo in sub-pipeline should be rejected",
    );
    assert.commandFailedWithCode(
        db.runCommand({
            create: jsTestName() + "_view",
            viewOn: coll.getName(),
            pipeline: lookupPipeline,
        }),
        kNotAllowedInLookupErrorCode,
    );
}
*/

// Test that a $unionWith pipeline can reject an extension stage.
{
    const unionWithPipeline = [{$unionWith: {coll: other.getName(), pipeline: [{$testFoo: {}}]}}];
    assertErrorCode(
        coll,
        unionWithPipeline,
        kNotAllowedInUnionWithErrorCode,
        "Using $unionWith with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$unionWith: {coll: other.getName(), pipeline: unionWithPipeline}}],
        kNotAllowedInUnionWithErrorCode,
        "Using $unionWith with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$lookup: {from: other.getName(), pipeline: unionWithPipeline, as: "joined"}}],
        kNotAllowedInUnionWithErrorCode,
        "Using $unionWith with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$facet: {facetPipe: unionWithPipeline}}],
        kNotAllowedInUnionWithErrorCode,
        "Using $unionWith with $testFoo in sub-pipeline should be rejected",
    );
    assert.commandFailedWithCode(
        db.runCommand({
            create: jsTestName() + "_view",
            viewOn: coll.getName(),
            pipeline: unionWithPipeline,
        }),
        kNotAllowedInUnionWithErrorCode,
    );
}
/*
TODO SERVER-117179 Enable this test.
// Test that a $facet pipeline can reject an extension stage.
{
    const facetPipeline = [{$facet: {facetPipe: [{$testFoo: {}}]}}];
    assertErrorCode(
        coll,
        facetPipeline,
        kNotAllowedInFacetErrorCode,
        "Using $facet with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$facet: {facetPipe: facetPipeline}}],
        kNotAllowedInFacetErrorCode,
        "Using $facet with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$unionWith: {coll: other.getName(), pipeline: facetPipeline}}],
        kNotAllowedInFacetErrorCode,
        "Using $facet with $testFoo in sub-pipeline should be rejected",
    );
    assertErrorCode(
        coll,
        [{$lookup: {from: other.getName(), pipeline: facetPipeline, as: "joined"}}],
        kNotAllowedInFacetErrorCode,
        "Using $facet with $testFoo in sub-pipeline should be rejected",
    );
    assert.commandFailedWithCode(
        db.runCommand({
            create: jsTestName() + "_view",
            viewOn: coll.getName(),
            pipeline: facetPipeline,
        }),
        kNotAllowedInFacetErrorCode,
    );
}
*/
