/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created. This test is focused on testing timeseries with sort and geo indexes.
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
 * ]
 */
import {
    runDoesntRewriteTest,
    setupColl
} from "jstests/core/timeseries/libs/timeseries_sort_util.js";

const geoCollName = 'bucket_unpacking_with_sort_with_geo';
const geoColl = db[geoCollName];

geoColl.drop();
// We'll only use the geo collection to test that the rewrite doesn't happen, so it doesn't
// need to be big.
assert.commandWorked(
    db.createCollection(geoCollName, {timeseries: {timeField: "t", metaField: "m"}}));
// This polygon is big enough that a 2dsphere index on it is multikey.
const area = {
    type: "Polygon",
    coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]
};
assert.commandWorked(geoColl.insert([
    // These two locations are far enough apart that a 2dsphere index on 'loc' is multikey.
    {t: ISODate('1970-01-01'), m: {area}, loc: [0, 0]},
    {t: ISODate('1970-01-01'), m: {area}, loc: [90, 0]},
]));
assert.eq(db['system.buckets.' + geoCollName].count(), 1);

// Geo indexes are typically multikey, which prevents us from doing the rewrite.
const indexes = [
    {t: 1, 'm.area': '2dsphere'},
    {'m.a': 1, t: 1, 'm.area': '2dsphere'},
    {t: 1, loc: '2dsphere'},
    {'m.a': 1, t: 1, loc: '2dsphere'},
];
for (const ix of indexes)
    runDoesntRewriteTest({t: 1}, ix, ix, geoColl, [{$match: {'m.a': 7}}]);
