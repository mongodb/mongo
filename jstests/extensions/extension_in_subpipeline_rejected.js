/**
 * Tests that using an extension stage in a sub-pipeline is rejected, for now.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.extension_in_subpipeline_rejected;
const other = db.extension_in_subpipeline_rejected_other;
coll.drop();
other.drop();

assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(other.insert({_id: 1}));

// Test $lookup pipeline rejects extension stage.
{
    const lookupPipeline =
        [{$lookup: {from: other.getName(), as: "joined", pipeline: [{$testFoo: {}}]}}];
    assertErrorCode(coll,
                    lookupPipeline,
                    51047,
                    "Using $lookup with $testFoo in sub-pipeline should be rejected");
}

// Test $unionWith pipeline rejects extension stage.
{
    const unionWithPipeline = [{$unionWith: {coll: other.getName(), pipeline: [{$testFoo: {}}]}}];
    assertErrorCode(coll,
                    unionWithPipeline,
                    31441,
                    "Using $unionWith with $testFoo in sub-pipeline should be rejected");
}

// Test $facet pipeline rejects extension stage.
{
    const facetPipeline = [{$facet: {facetPipe: [{$testFoo: {}}]}}];
    assertErrorCode(coll,
                    facetPipeline,
                    40600,
                    "Using $facet with $testFoo in sub-pipeline should be rejected");
}
