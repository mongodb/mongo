/**
 * Tests that SBE avoids building traverseF instructions in leading $match when path arrayness information is available.
 *
 * @tags: [
 *    assumes_against_mongod_not_mongos,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    assumes_unsharded_collection,
 *    # We modify the value of a query knob. setParameter is not persistent.
 *    does_not_support_stepdowns,
 *    # Explain for the aggregate command cannot run within a multi-document transaction
 *    does_not_support_transactions,
 *    assumes_no_implicit_index_creation,
 *    requires_fcv_90
 * ]
 */

import {getEngine, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function runTestCase(
    pipeline,
    assertFilterStages,
    aggOptions = {},
    {createClustered = false} = {},
) {
    jsTest.log(
        `Testing with internalQueryFrameworkControl: pipeline: ${tojson(pipeline)}, aggOptions: ${tojson(aggOptions)}, createClustered: ${createClustered}`,
    );

    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();
    if (createClustered) {
        assert.commandWorked(
            db.createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}),
        );
    }
    coll.insert({_id: 0, a: {b: 1, c: 1, "": 1}});
    coll.createIndex({"a.b": 1});
    coll.createIndex({"a.c": 1});
    coll.createIndex({"a.": 1});

    const explain = coll.explain("executionStats").aggregate(pipeline, aggOptions);
    const engine = getEngine(explain);

    if (engine === "sbe") {
        const filterStages = getSbePlanStages(explain, "filter");
        assert.gt(filterStages.length, 0, "Should have at least one filter stage");
        assertFilterStages(filterStages);

        if (createClustered) {
            const stages = getQueryPlanner(explain).winningPlan.slotBasedPlan.stages;
            assert(
                stages.includes("minRecordId") || stages.includes("maxRecordId"),
                `Expected a clustered scan (min/maxRecordId slot) in the SBE plan: ${stages}`,
            );
        }
    }

    coll.drop();
}

const sbeModes = ["trySbeRestricted", "trySbeEngine", "forceClassicEngine"];

function assertOnlyLeadingMatchAvoidsTraverseF(filterStages) {
    for (let i = 0; i < filterStages.length; i++) {
        if (i === filterStages.length - 1) {
            assert(
                !filterStages[i].filter.includes("traverseF"),
                `Leading $match should not use traverseF. Filter: ${filterStages[i].filter}`,
            );
        } else {
            assert(
                filterStages[i].filter.includes("traverseF"),
                `Non-leading $match should use traverseF. Filter: ${filterStages[i].filter}`,
            );
        }
    }
}

// Pipelines where traverseF elision applies to the leading $match.
const elisionPipelines = [
    [{$match: {"a.c": 1, "a.b": 1}}, {$group: {_id: null, tot: {$sum: "$a.c"}}}],
    [
        {$match: {"a.c": 1, "a.b": 1}},
        {$group: {_id: null, tot: {$sum: "$a.c"}}},
        {$addFields: {"a.b": "$tot"}},
        {$match: {"a.b": 1}},
    ],
];

// Paths with empty path components are excluded from elision, so the leading $match
// retains traverseF even when PathArrayness is available.
function assertLeadingMatchRetainsTraverseF(filterStages) {
    const leading = filterStages[filterStages.length - 1];
    assert(
        leading.filter.includes("traverseF"),
        `Leading $match with empty path component should retain traverseF. Filter: ${leading.filter}`,
    );
}

const emptyComponentPipelines = [
    [{$match: {"a.": 1, "a.b": 1}}, {$group: {_id: null, tot: {$sum: "$a.c"}}}],
    [{$match: {"a.c": 1, "a.": 1}}, {$group: {_id: null, tot: {$sum: "$a.c"}}}],
];

for (const pipeline of elisionPipelines) {
    runTestCase(pipeline, assertOnlyLeadingMatchAvoidsTraverseF);
}
for (const pipeline of emptyComponentPipelines) {
    runTestCase(pipeline, assertLeadingMatchRetainsTraverseF);
}

// Repeat the same assertions with a $natural hint to force a generic collection scan. The
// leading $match filter then runs on ScanStage rather than FetchStage; traverseF elision
// still applies because the filter sees raw documents from the main namespace.
const naturalHint = {hint: {$natural: 1}};
for (const pipeline of elisionPipelines) {
    runTestCase(pipeline, assertOnlyLeadingMatchAvoidsTraverseF, naturalHint);
}
for (const pipeline of emptyComponentPipelines) {
    runTestCase(pipeline, assertLeadingMatchRetainsTraverseF, naturalHint);
}

// Repeat for the clustered collection-scan path. An _id range predicate produces
// minRecord/maxRecord bounds, steering the planner to generateClusteredCollScan. The
// residual predicates ("a.b", "a.c", or "a.") remain under traverseF-elision rules.
function prefixIdBound(pipeline) {
    const [leading, ...rest] = pipeline;
    const match = Object.assign({_id: {$gte: 0}}, leading.$match);
    return [{$match: match}, ...rest];
}
for (const pipeline of elisionPipelines) {
    runTestCase(prefixIdBound(pipeline), assertOnlyLeadingMatchAvoidsTraverseF, naturalHint, {
        createClustered: true,
    });
}
for (const pipeline of emptyComponentPipelines) {
    runTestCase(prefixIdBound(pipeline), assertLeadingMatchRetainsTraverseF, naturalHint, {
        createClustered: true,
    });
}
