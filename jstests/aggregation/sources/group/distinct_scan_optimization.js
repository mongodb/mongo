/**
 * This is a property-based test for the group distinct scan optimization. It works by generating an
 * index spec and a set of documents and using that index to construct a pipeline which will perform
 * a distinct scan.
 *
 * @tags: [
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   requires_pipeline_optimization,
 *   # We don't want to verify that the optimization is applied inside $facet since its shape is
 *   # quite different from the original one.
 *   do_not_wrap_aggregations_in_facets,
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {assertPlanUsesDistinctScan} from "jstests/libs/query/group_to_distinct_scan_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const coll = db.distinct_scan_optimization;
coll.drop();

const shardKey = (() => {
    try {
        const shardKeyFields = Object.keys(coll.getShardKey());
        assert.eq(1, shardKeyFields.length);
        return shardKeyFields[0];
    } catch (e) {
        return undefined;
    }
})();

// Bail out of the test if featureFlagShardFilteringDistinctScan is disabled.
if (shardKey !== undefined && !FeatureFlagUtil.isEnabled(db, "ShardFilteringDistinctScan")) {
    jsTestLog(
        "Skipping distinct_scan_optimization.js because we encountered sharded collection and featureFlagShardFilteringDistinctScan is disabled");
    quit();
}

// Areas for improvement of this test:
// * Add dotted paths
// * Support $topN and $bottomN accumulators and sortBy field
// * Generate multiple valid indexes so that we stress the distinct scan multiplanning code

const fieldArb = fc.constantFrom('_id', 'a', 'b', 'c', 'mk');
const directionArb = fc.constantFrom(1, -1);
const accumArb = fc.constantFrom("$first", "$last");
// Arbitrary representing an index spec that is eligible for the group distinct scan optimization.
const indexSpecArb =
    fc.tuple(
          fc.uniqueArray(fieldArb, {minLength: 2, maxLength: 5}),
          directionArb,  // Direction for first field
          // TODO SERVER-95418: Remove this restriction when mixture of $first and $last is allowed
          directionArb  // Direction for rest of the fields
          )
        .map(([fields, firstDirection, restDirection]) => {
            const obj = {};
            fields.forEach((field, index) => {
                obj[field] = index === 0 ? firstDirection : restDirection;
            });
            return obj;
        });
// Arbitrary representing values for RHS. This set is small to keep the minimization time fast and
// ensure that each bucket in a $group has multiple entries.
const fieldValueArb =
    fc.oneof(fc.integer({min: 1, max: 3}), fc.constantFrom("aaa", "bbb", "ccc"), fc.constant(null));

// Arbitrary like 'fieldValueArb' which cannot be null. Used for $match stage before $sort.
// Comparison to null prevents the distinct scan optimization.
const nonNullFieldArb =
    fc.oneof(fc.integer({min: 1, max: 3}), fc.constantFrom("aaa", "bbb", "ccc"));

// Arbitrary representing an array of integers.
const multikeyValueArb = fc.array(fc.integer(), {maxLength: 3});

// Arbitrary representing a document.
const documentArb = fc.record({
    a: fieldValueArb,
    b: fieldValueArb,
    c: fieldValueArb,
    // Allow for one multikey field
    mk: multikeyValueArb,
},
                              {
                                  noNullPrototype: false,
                              });

// Arbitrary for all documents in the collection.
const docsArb = fc.array(documentArb, {minLength: 30, maxLength: 50});

// Arbitrary for whether to include a match stage.
const includeMatchArb = fc.boolean();

// Arbitrary for a single test case.
const testCaseArb = fc.tuple(indexSpecArb, accumArb, docsArb, includeMatchArb, nonNullFieldArb);

// Takes an array of integers and returns a boolean indicating whether it contains duplicates.
function hasDuplicates(arr) {
    return new Set(arr).size !== arr.length;
}

fc.assert(fc.property(testCaseArb, ([indexSpec, accumOp, docs, includeMatch, matchValue]) => {
    const fields = Object.keys(indexSpec);
    // The distinct scan optimization doesn't work when the group key is multikey.
    fc.pre(fields[0] !== "mk");

    // If we have a shard key, it must be the group key to generate a distinct scan plan.
    if (shardKey !== undefined) {
        fc.pre(fields[0] === shardKey);
    }

    // Only the $first accumulator works with multikey fields.
    //
    // Suppose we have the query:
    // [{$sort: {a: 1, mk: 1}}, {$group: {_id: '$a', accum: {$last: '$mk'}}}]
    // This sort means we want to order documents in ascending order of the the smallest value in
    // the mk array. We cannot get the last such element by using a distinct scan on the {a: 1, mk:
    // 1} index in backwards direction because the index contains keys for all values of the mk
    // array. The first one we'd encounter in a backwards index scan may not correspond to the
    // document containing the largest smallest value in an mk array.
    //
    // Supose we have the other case:
    // [{$sort: {a: 1, mk: -1}}, {$group: {_id: '$a', accum: {$last: '$mk'}}}]
    // This sort order means order the documents by descending values of the largest element in mk.
    // The same logic as described above applies.
    if (indexSpec.hasOwnProperty("mk")) {
        fc.pre(accumOp === "$first");
    }

    // Ensure that all values in the mk array across all documents are unique. This prevents a
    // situation with tied sort order, which can result in multiple correct answers.
    // For example:
    // * Index: {a: 1, mk: 1}
    // * Docs: {_id: 1, a: 1, mk: [1,2,3]}, {_id: 2, a: 1, mk: [1]}
    // * Query: {$sort: {a: 1, mk: 1}}, {$group: {_id: '$a', accum: {$first: '$mk'}}}
    // Both {_id: 1, accum: [1]} and {_id: 1, accum: [1,2,3]} are valid results.
    fc.pre(!hasDuplicates(docs.map(doc => doc.mk).flat()));

    coll.drop();
    assert.commandWorked(coll.insert(docs));

    const sort = {$sort: indexSpec};
    // Distinct scan only supports grouping over a single field.
    // TODO SERVER-96679: Remove this restriction.
    const groupId = {_id: `$${fields[0]}`};
    let accumObj = {};
    for (let i = 1; i < fields.length; i++) {
        const curField = fields[i];
        accumObj[`${curField}_accum`] = {[accumOp]: `$${curField}`};
    }
    let pipeline = [sort, {$group: {...groupId, ...accumObj}}];
    if (includeMatch) {
        const match = {
            '$match': {
                [fields[0]]: {$gte: matchValue},
            },
        };
        pipeline = [match, ...pipeline];
    }

    jsTestLog(docs);
    jsTestLog(indexSpec);
    jsTestLog(pipeline);

    assert.commandWorked(coll.createIndex(indexSpec));
    assertPlanUsesDistinctScan(db, coll.explain().aggregate(pipeline));

    const ixScanRes = coll.aggregate(pipeline).toArray();
    const collScanRes = coll.aggregate(pipeline, {hint: {$natural: 1}}).toArray();

    assert(_resultSetsEqualUnordered(ixScanRes, collScanRes));
}), {seed: 5, numRuns: 500});
