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
 *    featureFlagPathArrayness
 * ]
 */

import {getEngine, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function runTestCase(pipeline, assertFilterStages) {
    jsTest.log(`Testing with internalQueryFrameworkControl: pipeline: ${tojson(pipeline)}`);

    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();
    coll.insert({a: {b: 1, c: 1, "": 1}});
    coll.createIndex({"a.b": 1});
    coll.createIndex({"a.c": 1});
    coll.createIndex({"a.": 1});

    const explain = coll.explain("executionStats").aggregate(pipeline);
    const engine = getEngine(explain);

    if (engine === "sbe") {
        const filterStages = getSbePlanStages(explain, "filter");
        assert.gt(filterStages.length, 0, "Should have at least one filter stage");
        assertFilterStages(filterStages);
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
