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
    {
        const viewName = "testBar_in_def";
        assert.commandWorked(db.createView(viewName, other.getName(), [{$testBar: {noop: false}}]));

        assertErrorCode(
            coll,
            [{$lookup: {from: viewName, pipeline: [], as: "joined"}}],
            kNotAllowedInLookupErrorCode,
            "Using $lookup on view with $testBar in view definition should be rejected",
        );

        db[viewName].drop();
    }
    {
        const viewName = "matchTopN_in_def";
        assert.commandWorked(
            db.createView(viewName, other.getName(), [
                {$addFields: {type: "A"}},
                {$matchTopN: {filter: {}, sort: {_id: 1}, limit: 1}},
            ]),
        );

        assertErrorCode(
            coll,
            [{$lookup: {from: viewName, pipeline: [], as: "joined"}}],
            kNotAllowedInLookupErrorCode,
            "Using $lookup on view with $matchTopN in view definition should be rejected",
        );

        db[viewName].drop();
    }
}

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
    {
        const viewName = "testFoo_in_def";
        assert.commandWorked(db.createView(viewName, other.getName(), [{$testFoo: {}}]));

        assertErrorCode(
            coll,
            [{$unionWith: {coll: viewName, pipeline: []}}],
            kNotAllowedInUnionWithErrorCode,
            "Using $unionWith on view with $testFoo in view definition should be rejected",
        );

        db[viewName].drop();
    }
}

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
    assertErrorCode(
        coll,
        [{$facet: {facetPipe: [{$testBar: {noop: true}}]}}],
        kNotAllowedInFacetErrorCode,
        "Using $facet with $testBar in sub-pipeline should be rejected",
    );

    assertErrorCode(
        coll,
        [{$facet: {facetPipe: [{$matchTopN: {filter: {}, sort: {_id: 1}, limit: 5}}]}}],
        kNotAllowedInFacetErrorCode,
        "Using $facet with $matchTopN (desugar) in sub-pipeline should be rejected",
    );

    // TODO SERVER-118956 Re-enable this test.
    /**
    {
        const viewName = "unionwith_target";
        assert.commandWorked(
            db.createView(viewName, other.getName(), [{$testBar: {noop: true}}, {$match: {type: "X"}}]),
        );

        
        assertErrorCode(
            coll,
            [
                {
                    $facet: {
                        withUnion: [{$unionWith: viewName}, {$sort: {_id: 1}}],
                    },
                },
            ],
            kNotAllowedInFacetErrorCode,
            "Using $facet.$unionWith with $testBar in $unionWith view should be rejected",
        );

        db[viewName].drop();
    }
    */
}

// =============================================================================
// Test $lookup on nested view chains where any view has extension
// =============================================================================

// Helper to test $lookup rejection on view chains with extensions.
function testLookupOnViewChainRejected(viewSpecs, description) {
    const viewNames = [];
    let sourceColl = other.getName();

    // Create view chain.
    for (const spec of viewSpecs) {
        assert.commandWorked(db.createView(spec.name, sourceColl, spec.pipeline));
        viewNames.push(spec.name);
        sourceColl = spec.name;
    }

    // Test $lookup on the top view.
    const topViewName = viewNames[viewNames.length - 1];
    assertErrorCode(
        coll,
        [{$lookup: {from: topViewName, pipeline: [], as: "joined"}}],
        kNotAllowedInLookupErrorCode,
        description,
    );

    // Cleanup in reverse order.
    for (let i = viewNames.length - 1; i >= 0; i--) {
        db[viewNames[i]].drop();
    }
}

{
    // Test $lookup on a view chain where the base view has an extension.
    testLookupOnViewChainRejected(
        [
            {name: "nested_base_with_ext", pipeline: [{$testBar: {noop: true}}]},
            {name: "nested_top_no_ext", pipeline: [{$addFields: {fromTopView: true}}]},
        ],
        "Using $lookup on view chain where base view has extension should be rejected",
    );

    // Test $lookup on a 3-level view chain where middle view has extension.
    testLookupOnViewChainRejected(
        [
            {name: "chain_level1_no_ext", pipeline: [{$addFields: {level1: true}}]},
            {name: "chain_level2_with_ext", pipeline: [{$testBar: {noop: true}}, {$addFields: {level2: true}}]},
            {name: "chain_level3_no_ext", pipeline: [{$addFields: {level3: true}}]},
        ],
        "Using $lookup on 3-level view chain where middle view has extension should be rejected",
    );

    // Test $lookup on view chain with desugaring extension ($matchTopN).
    testLookupOnViewChainRejected(
        [
            {name: "chain_desugar_base", pipeline: [{$matchTopN: {filter: {}, sort: {_id: 1}, limit: 5}}]},
            {name: "chain_desugar_top", pipeline: [{$addFields: {top: true}}]},
        ],
        "Using $lookup on view chain where base view has desugaring extension should be rejected",
    );
}

// =============================================================================
// Test cross-stage rejection scenarios
// =============================================================================

// Helper to test cross-stage rejection with a single extension view.
function testCrossStageRejection(viewName, viewPipeline, testPipeline, expectedErrorCode, description) {
    assert.commandWorked(db.createView(viewName, other.getName(), viewPipeline));
    assertErrorCode(coll, testPipeline, expectedErrorCode, description);
    db[viewName].drop();
}

{
    // $facet containing $lookup on view with extension.
    testCrossStageRejection(
        "facet_lookup_view_ext",
        [{$testBar: {noop: true}}],
        [{$facet: {facetPipe: [{$lookup: {from: "facet_lookup_view_ext", pipeline: [], as: "joined"}}]}}],
        kNotAllowedInLookupErrorCode,
        "Using $facet with $lookup on view with extension should be rejected",
    );

    // Nested $lookup in $unionWith targeting view with extension.
    testCrossStageRejection(
        "unionwith_nested_lookup_ext",
        [{$testBar: {noop: true}}],
        [
            {
                $unionWith: {
                    coll: other.getName(),
                    pipeline: [{$lookup: {from: "unionwith_nested_lookup_ext", pipeline: [], as: "joined"}}],
                },
            },
        ],
        kNotAllowedInLookupErrorCode,
        "Using $unionWith containing $lookup on view with extension should be rejected",
    );

    // $lookup pipeline with $unionWith targeting view with extension.
    testCrossStageRejection(
        "lookup_unionwith_ext_view",
        [{$testBar: {noop: true}}],
        [{$lookup: {from: other.getName(), pipeline: [{$unionWith: "lookup_unionwith_ext_view"}], as: "joined"}}],
        kNotAllowedInLookupErrorCode,
        "Using $lookup pipeline with $unionWith targeting extension view should be rejected",
    );

    // $lookup on view whose definition contains $unionWith targeting view with extension.
    {
        const innerViewName = "inner_ext_view";
        const outerViewName = "outer_with_unionwith";

        assert.commandWorked(db.createView(innerViewName, other.getName(), [{$testBar: {noop: true}}]));
        assert.commandWorked(db.createView(outerViewName, other.getName(), [{$unionWith: innerViewName}]));

        assertErrorCode(
            coll,
            [{$lookup: {from: outerViewName, pipeline: [], as: "joined"}}],
            kNotAllowedInLookupErrorCode,
            "Using $lookup on view with $unionWith targeting extension view should be rejected",
        );

        db[outerViewName].drop();
        db[innerViewName].drop();
    }
}
