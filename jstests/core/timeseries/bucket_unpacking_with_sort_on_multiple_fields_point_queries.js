/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created. This test is focused on timeseries with sort on multiple fields point
 * queries.
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
    runRewritesTest,
    setupColl
} from "jstests/core/timeseries/libs/timeseries_sort_util.js";

const metaCollSubFieldsName = jsTestName();
const metaCollSubFields = db[metaCollSubFieldsName];
const subFields = ["a", "b"];

setupColl(metaCollSubFields, metaCollSubFieldsName, true, subFields);

// Test point predicate on multiple meta fields.
for (const sort of [-1, +1]) {
    for (const a of [-1, +1]) {
        for (const b of [-1, +1]) {
            for (const t of [-1, +1]) {
                for (const trailing of [{}, {x: 1, y: -1}]) {
                    const index = Object.merge({'m.a': a, 'm.b': b, t: t}, trailing);
                    const expectedAccessPath = t === sort ? forwardIxscan : backwardIxscan;
                    runRewritesTest({t: sort},
                                    index,
                                    index,
                                    expectedAccessPath,
                                    metaCollSubFields,
                                    t === sort,
                                    [{$match: {'m.a': 5, 'm.b': 5}}]);
                    runRewritesTest(
                        {t: sort}, index, null, expectedAccessPath, metaCollSubFields, t === sort, [
                            {$match: {'m.a': 5, 'm.b': 5}}
                        ]);
                }
            }
        }
    }
}
