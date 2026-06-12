/**
 * A property-based test for $sort+$group pipelines with $top/$bottom accumulators. Targets cases
 * where the distinct scan optimization can be applied incorrectly (SERVER-110803).
 * When the accumulator's sortBy direction is opposite to the $sort direction for the
 * field immediately following the group key in the index, the optimization uses the wrong scan
 * direction.
 * Correctness is verified by comparing index-based execution against a forced collection scan,
 * which bypasses all index/distinct-scan optimizations and serves as the ground truth.
 *
 * Document generation strategy:
 *   - Group key field uses a small domain (1-3 or null) so each group contains multiple docs.
 *   - The first sort-by field uses globally unique integers (1-10000) so no two docs in any group
 *     share the same sort key, guaranteeing no ties without a precondition filter.
 *   - Remaining fields use a moderate random domain.
 *
 * @tags: [
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   requires_pipeline_optimization,
 *   # We don't want to verify that the optimization is applied inside $facet since its shape is
 *   # quite different from the original one.
 *   do_not_wrap_aggregations_in_facets,
 *   cannot_run_during_upgrade_downgrade,
 *   # $top/$bottom distinct scan optimization landed in 8.0,
 *   requires_fcv_80,
 *   # PBT infrastructure uses commands not compatible with the simulate_mongoq override.
 *   simulate_mongoq_incompatible,
 *   # Burn-in confirmed no sharded passthrough shards on the test document fields (a, b, c),
 *   # so the distinct scan optimization is never exercisable on a sharded collection.
 *   assumes_unsharded_collection,
 *   query_intensive_pbt,
 * ]
 */
import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const controlColl = db.sort_group_with_top_bottom_pbt_control;
const experimentColl = db.sort_group_with_top_bottom_pbt;

const allDocFields = ["a", "b", "c"];

const fieldArb = fc.constantFrom(...allDocFields);
const directionArb = fc.constantFrom(1, -1);
const accumTypeArb = fc.constantFrom("$top", "$bottom");

// Produces an index spec with 2-3 unique fields, each with an independently chosen direction.
// Example: {a: 1, b: -1} or {a: -1, b: 1, c: 1}.
const indexSpecArb = fc.uniqueArray(fieldArb, {minLength: 2, maxLength: 3}).chain((fields) => {
    return fc.tuple(...fields.map(() => directionArb)).map((dirs) => {
        const spec = {};
        fields.forEach((f, i) => {
            spec[f] = dirs[i];
        });
        return spec;
    });
});

// The full test case is chained from the index spec so the document generator can assign
// different value domains to different fields based on their role.
const testCaseArb = indexSpecArb.chain((indexSpec) => {
    const fields = Object.keys(indexSpec);
    const groupKey = fields[0]; // small domain → multiple docs per group
    const firstSortField = fields[1]; // globally unique integers → no ties guaranteed

    // Generate a single flip factor (1 or -1) applied uniformly to all non-group-key fields.
    // This ensures sortBy directions are either all-same or all-opposite relative to the index
    const remainingFields = fields.slice(1);
    const sortByDirsArb = directionArb.map((flip) =>
        remainingFields.map((f) => indexSpec[f] * flip),
    );

    // Generate numDocs documents with guaranteed tie-free sort keys.
    const docsArb = fc.integer({min: 20, max: 40}).chain((numDocs) => {
        // Globally unique integers for firstSortField: within any group these are also unique,
        // so the $top/$bottom sortBy result is always deterministic — no fc.pre needed.
        const uniqueSortValsArb = fc.uniqueArray(fc.integer({min: 1, max: 10000}), {
            minLength: numDocs,
            maxLength: numDocs,
        });

        // Small domain for group key so each group gets several documents.
        const groupKeyValsArb = fc.array(
            fc.oneof(fc.integer({min: 1, max: 3}), fc.constant(null)),
            {
                minLength: numDocs,
                maxLength: numDocs,
            },
        );

        // Moderate domain for every field not yet assigned (extra sort field + non-index fields).
        const extraFieldVal = fc.oneof(
            fc.integer({min: 1, max: 10}),
            fc.constantFrom("x", "y", "z"),
        );
        const extraFieldsArb = allDocFields
            .filter((f) => f !== groupKey && f !== firstSortField)
            .reduce((acc, f) => {
                acc[f] = extraFieldVal;
                return acc;
            }, {});
        const extraDocsArb = fc.array(fc.record(extraFieldsArb), {
            minLength: numDocs,
            maxLength: numDocs,
        });

        return fc
            .tuple(uniqueSortValsArb, groupKeyValsArb, extraDocsArb)
            .map(([sortVals, gkVals, extraDocs]) =>
                sortVals.map((sv, i) => ({
                    [groupKey]: gkVals[i],
                    [firstSortField]: sv,
                    ...extraDocs[i],
                })),
            );
    });

    return fc.tuple(fc.constant(indexSpec), accumTypeArb, sortByDirsArb, docsArb);
});

// Maps our generated tuple into the {collSpec, queries, extraParams} shape expected by testProperty.
const workloadModel = testCaseArb.map(([indexSpec, accumType, sortByDirs, docs]) => {
    const fields = Object.keys(indexSpec);
    const groupKey = fields[0];
    const outputField = fields[1];
    const remainingFields = fields.slice(1);

    const sortBySpec = {};
    remainingFields.forEach((f, i) => {
        sortBySpec[f] = sortByDirs[i];
    });

    const pipeline = [
        {$sort: indexSpec},
        {
            $group: {
                _id: `$${groupKey}`,
                result: {[accumType]: {output: `$${outputField}`, sortBy: sortBySpec}},
            },
        },
    ];

    return {
        collSpec: {docs, indexes: [{def: indexSpec, options: {}}]},
        queries: [{pipeline, options: {hint: indexSpec}}],
        extraParams: {indexSpec},
    };
});

const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl, {
    preconditionFn(explainResult) {
        // Verify DISTINCT_SCAN fires before comparing results: if it doesn't, both paths are
        // collection scans and will always agree, giving the test no regression value.
        fc.pre(getAggPlanStages(explainResult, "DISTINCT_SCAN").length > 0);
    },
});

testProperty(correctnessProperty, {controlColl, experimentColl}, workloadModel, 150 /*numRuns*/);
