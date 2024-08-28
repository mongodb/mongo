/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created. This test is focused on timeseries with compound sort on point queries.
 *
 * @tags: [
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 *     # This complicates aggregation extraction.
 *     do_not_wrap_aggregations_in_facets,
 *     # Refusing to run a test that issues an aggregation command with explain because it may
 *     # return incomplete results if interrupted by a stepdown.
 *     does_not_support_stepdowns,
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     # TODO (SERVER-88539) the timeseries setup runs a migration. Remove the upgrade-downgrade
 *     # incompatible tag once migrations  work during downgrade.
 *     cannot_run_during_upgrade_downgrade,
 *     requires_getmore,
 * ]
 */
import {
    backwardIxscan,
    forwardIxscan,
    runDoesntRewriteTest,
    runRewritesTest,
    setupColl
} from "jstests/core/timeseries/libs/timeseries_sort_util.js";

const metaCollSubFieldsName = "bucket_unpacking_with_compound_sort_with_meta_sub_on_point_queries";
const metaCollSubFields = db[metaCollSubFieldsName];
const subFields = ["a", "b"];

setupColl(metaCollSubFields, metaCollSubFieldsName, true, subFields);

// Test mixed cases involving both a point predicate and compound sort.
// In all of these cases we have an index on {m.a, m.b, t}, and possibly some more trailing fields.
for (const ixA of [-1, +1]) {
    for (const ixB of [-1, +1]) {
        for (const ixT of [-1, +1]) {
            for (const ixTrailing of [{}, {x: 1, y: -1}]) {
                const ix = Object.merge({'m.a': ixA, 'm.b': ixB, t: ixT}, ixTrailing);

                // Test a point predicate on 'm.a' with a sort on {m.b, t}.
                // The point predicate lets us zoom in on a contiguous range of the index,
                // as if we were using an index on {constant, m.b, t}.
                for (const sortB of [-1, +1]) {
                    for (const sortT of [-1, +1]) {
                        const predicate = [{$match: {'m.a': 7}}];
                        const sort = {'m.b': sortB, t: sortT};

                        // 'sortB * sortT' is +1 if the sort has those fields in the same
                        // direction, -1 for opposite direction. 'b * t' says the same thing about
                        // the index key. The index and sort are compatible iff they agree on
                        // whether or not these two fields are in the same direction.
                        if (ixB * ixT === sortB * sortT) {
                            runRewritesTest(
                                sort, ix, ix, null, metaCollSubFields, ixT === sortT, predicate);
                            runRewritesTest(sort,
                                            ix,
                                            null,
                                            ixT === sortT ? forwardIxscan : backwardIxscan,
                                            metaCollSubFields,
                                            ixT === sortT,
                                            predicate);
                        } else {
                            runDoesntRewriteTest(sort, ix, ix, metaCollSubFields, predicate);
                        }
                    }
                }

                // Test a point predicate on 'm.b' with a sort on {m.a, t}.
                // This predicate does not select a contiguous range of the index, but it does
                // limit the scan to index entries that look like {m.a, constant, t}, which can
                // satisfy a sort on {m.a, t}.
                for (const sortA of [-1, +1]) {
                    for (const sortT of [-1, +1]) {
                        const sort = {'m.a': sortA, t: sortT};

                        // However, when there is no point predicate on 'm.a', the planner gives us
                        // a full index scan with no bounds on 'm.b'. Since our implementation
                        // looks at index bounds to decide whether to rewrite, we don't get the
                        // optimization in this case.
                        {
                            const predicate = [{$match: {'m.b': 7}}];
                            runDoesntRewriteTest(sort, ix, ix, metaCollSubFields, predicate);
                        }

                        // We do get the optimization if we add any range predicate to 'm.a',
                        // because that makes the planner generate index bounds: a range on 'm.a'
                        // and a single point on 'm.b'.
                        //
                        // As usual the index and sort must agree on whether m.a, t are
                        // in the same direction.
                        const predicate = [{$match: {'m.a': {$gte: -999, $lte: 999}, 'm.b': 7}}];
                        if (ixA * ixT === sortA * sortT) {
                            runRewritesTest(
                                sort, ix, ix, null, metaCollSubFields, ixT === sortT, predicate);
                            runRewritesTest(sort,
                                            ix,
                                            null,
                                            ixT === sortT ? forwardIxscan : backwardIxscan,
                                            metaCollSubFields,
                                            ixT === sortT,
                                            predicate);
                        } else {
                            runDoesntRewriteTest(sort, ix, ix, metaCollSubFields, predicate);
                        }
                    }
                }
            }
        }
    }
}
