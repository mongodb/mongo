/**
 * Compares SBE streaming $group with SBE hash $group over the same hinted, ordered input.
 * Generated workloads contain three eligible key shapes and one deliberately ineligible shape.
 * Assert the lowered stages, rather than discarding examples that unexpectedly do not stream.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_fcv_91,
 *   requires_sbe,
 *   uses_explain,
 *   requires_getmore,
 *   requires_pipeline_optimization,
 *   do_not_wrap_aggregations_in_facets,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   assumes_stable_shard_list,
 *   # Initial scope: complete groups on an unsharded collection, including through mongos.
 *   assumes_unsharded_collection,
 *   exclude_from_timeseries_crud_passthrough,
 *   simulate_mongoq_incompatible,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getDifferentlyShapedQueries} from "jstests/libs/property_test_helpers/common_properties.js";
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getNestedProperties, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db) || !checkSbeRestrictedOrFullyEnabled(db)) {
    jsTest.log.info("Skipping streaming-group PBT: requires an optimized, non-sanitized SBE build");
    quit();
}

// Keep this allowlist small while streaming group is ported to the compact accumulators.
// Each enabled accumulator must have a corresponding input expression below.
const supportedAccumulators = ["$sum", "$push"];
const accumulatorInputs = {$sum: "$value", $push: "$$ROOT"};
const numRuns = 100;
const coll = db[jsTestName()];

const keyArb = fc.integer({min: -2, max: 2});
const docsArb = getDatasetModel({
    maxNumDocs: 40,
    docModel: fc.record({
        a: keyArb,
        b: keyArb,
        nested: fc.record({k: keyArb}),
        value: oneof(
            fc.integer({min: -100, max: 100}),
            fc.constant(null),
            fc.string({maxLength: 8}),
            fc.array(keyArb, {maxLength: 3}),
        ),
    }),
});

function makeWorkload({
    fields,
    directions,
    keyCount,
    accumulators,
    negative,
    docs,
    reverse = false,
}) {
    const index = Object.fromEntries([...fields, "_id"].map((f, i) => [f, directions[i]]));
    const sort = Object.fromEntries(
        Object.entries(index).map(([field, direction]) => [
            field,
            reverse ? -direction : direction,
        ]),
    );
    const accumulatedFields = Object.fromEntries(
        accumulators.map((acc, i) => [`acc${i}`, {[acc]: accumulatorInputs[acc]}]),
    );
    const compoundKey = Object.fromEntries(
        fields.slice(0, keyCount).map((field, i) => [`k${i}`, `$${field}`]),
    );
    const keys = [
        `$${fields[0]}`,
        {k: `$${fields[0]}`},
        compoundKey,
        negative === "computed" ? {$add: [`$${fields[0]}`, 1]} : `$${fields[1]}`,
    ];
    return {
        collSpec: {docs, indexes: [{def: index, options: {}}]},
        queries: keys.map((_id) => ({
            // _id is a unique tie-breaker, so even $push of nested arrays has an exact oracle.
            pipeline: [{$sort: sort}, {$group: {_id, ...accumulatedFields}}],
            options: {hint: index, allowDiskUse: false, cursor: {batchSize: 1}},
        })),
        extraParams: {eligible: [true, true, true, false], negative},
    };
}

const workloadArb = fc
    .record({
        fields: fc.shuffledSubarray(["a", "b", "nested.k"], {minLength: 3, maxLength: 3}),
        directions: fc.array(fc.constantFrom(1, -1), {minLength: 4, maxLength: 4}),
        keyCount: fc.integer({min: 1, max: 3}),
        accumulators: fc.uniqueArray(fc.constantFrom(...supportedAccumulators), {
            minLength: 1,
            maxLength: supportedAccumulators.length,
        }),
        negative: fc.constantFrom("computed", "nonPrefix"),
        reverse: fc.boolean(),
        docs: docsArb,
    })
    .map(makeWorkload);

// Pin single-document, one-large-group, and many-singleton-group boundary cases independently of
// the random seed. All use both accumulator types, including ordered, nested $push values.
const examples = [1, 2, 20].map((size) => [
    makeWorkload({
        fields: ["a", "nested.k", "b"],
        directions: [1, -1, 1, -1],
        keyCount: 2,
        accumulators: supportedAccumulators,
        negative: "nonPrefix",
        reverse: size % 2 === 0,
        docs: Array.from({length: size}, (_, i) => ({
            _id: i,
            a: 0,
            b: i,
            nested: {k: i},
            value: i % 2 ? [[i, -i], []] : i,
        })),
    }),
]);

function assertGroupStages(explain, streaming) {
    const stages = getQueryPlanners(explain)
        .flatMap((planner) => getNestedProperties(planner.winningPlan, "slotBasedPlan"))
        .map((plan) => plan.stages)
        .join("\n");
    assert.eq(
        streaming ? 1 : 0,
        (stages.match(/(?<!\w)streaming_group\s*\[/g) ?? []).length,
        "Unexpected number of lowered streaming groups",
        {explain},
    );
    assert.eq(
        streaming ? 0 : 1,
        (stages.match(/(?<!\w)(?:compact_)?group\s*\[/g) ?? []).length,
        "Expected exactly one SBE group, using hash aggregation when not streaming",
        {explain},
    );
}

describe("SBE streaming-group property", function () {
    let nodes;
    before(function () {
        nodes = DiscoverTopology.findNonConfigNodes(db.getMongo()).map((host) => new Mongo(host));
    });
    after(function () {
        for (const node of nodes ?? []) {
            node.close();
        }
    });

    function withStreamingGroup(enabled, fn) {
        // Feature-flag getParameter returns an object; setParameter needs its boolean 'value'.
        // Save each node independently, including secondaries used by read-preference overrides.
        const previous = nodes.map((node) => ({
            node,
            enabled: assert.commandWorked(
                node.adminCommand({getParameter: 1, featureFlagSbeStreamingGroup: 1}),
            ).featureFlagSbeStreamingGroup.value,
        }));
        try {
            for (const node of nodes) {
                assert.commandWorked(
                    node.adminCommand({setParameter: 1, featureFlagSbeStreamingGroup: enabled}),
                );
            }
            return fn();
        } finally {
            for (const {node, enabled} of previous) {
                assert.commandWorked(
                    node.adminCommand({setParameter: 1, featureFlagSbeStreamingGroup: enabled}),
                );
            }
        }
    }

    it("matches hash-group results and selects streaming only for eligible keys", function () {
        let completed = 0;
        function streamingMatchesHash(getQuery, helpers, {eligible, negative}) {
            const queries = getDifferentlyShapedQueries(getQuery, helpers);
            function execute(streamingEnabled) {
                return withStreamingGroup(streamingEnabled, () =>
                    queries.map(({pipeline, options}, i) => {
                        const explain = coll.explain().aggregate(pipeline, options);
                        assertGroupStages(explain, streamingEnabled && eligible[i]);
                        return coll.aggregate(pipeline, options).toArray();
                    }),
                );
            }
            const hashResults = execute(false);
            const streamingResults = execute(true);
            for (let i = 0; i < queries.length; ++i) {
                // Ignore only result-document order: $push element order and nested arrays matter.
                if (!helpers.comp(hashResults[i], streamingResults[i])) {
                    return {
                        passed: false,
                        query: queries[i],
                        eligible: eligible[i],
                        negative,
                        hashResults: hashResults[i],
                        streamingResults: streamingResults[i],
                    };
                }
            }
            ++completed;
            return {passed: true};
        }

        const previousTraceExceptions = TestData.traceExceptions;
        try {
            testProperty(
                streamingMatchesHash,
                {experimentColl: coll},
                workloadArb,
                numRuns,
                examples,
            );
            // The shared PBT runner may stop early after passing runs when its time budget expires.
            assert.gt(completed, 0, "Property must execute at least one workload");
            jsTest.log.info("Completed streaming-group property workloads", {completed});
        } finally {
            assertDropCollection(db, coll.getName());
            if (previousTraceExceptions === undefined) {
                delete TestData.traceExceptions;
            } else {
                TestData.traceExceptions = previousTraceExceptions;
            }
        }
    });
});
