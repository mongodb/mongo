/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created. This test is focused on testing timeseries with sort and collation.
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
    forwardIxscan,
    runDoesntRewriteTest,
    runRewritesTest
} from "jstests/core/timeseries/libs/timeseries_sort_util.js";

const ciStringCollName = jsTestName() + '_ci';
const ciStringColl = db[ciStringCollName];
const csStringCollName = jsTestName() + '_cs';
const csStringColl = db[csStringCollName];

// Create two collections, with the same data but different collation.

const times = [
    ISODate('1970-01-01T00:00:00'),
    ISODate('1970-01-01T00:00:07'),
];
let docs = [];
for (const m of ['a', 'A', 'b', 'B'])
    for (const t of times)
        docs.push({t, m});

csStringColl.drop();
ciStringColl.drop();
assert.commandWorked(db.createCollection(csStringCollName, {
    timeseries: {timeField: "t", metaField: "m"},
}));
assert.commandWorked(db.createCollection(ciStringCollName, {
    timeseries: {timeField: "t", metaField: "m"},
    collation: {locale: 'en_US', strength: 2},
}));

for (const coll of [csStringColl, ciStringColl]) {
    assert.commandWorked(coll.insert(docs));
    assert.eq(db['system.buckets.' + coll.getName()].count(), 4);
}

// String collation affects whether an equality query is really a point query.
//
// When the collation of the query matches the index, an equality predicate in the query
// becomes a 1-point interval in the index bounds.
runRewritesTest(
    {t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, forwardIxscan, csStringColl, true, [{$match: {m: 'a'}}]);
runRewritesTest(
    {t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, forwardIxscan, ciStringColl, true, [{$match: {m: 'a'}}]);
// When the collation doesn't match, then the equality predicate is not a 1-point interval
// in the index.
csStringColl.dropIndexes();
ciStringColl.dropIndexes();
assert.commandWorked(csStringColl.createIndex({m: 1, t: 1}, {
    collation: {locale: 'en_US', strength: 2},
}));
assert.commandWorked(ciStringColl.createIndex({m: 1, t: 1}, {
    collation: {locale: 'simple'},
}));
runDoesntRewriteTest({t: 1}, null, {m: 1, t: 1}, csStringColl, [{$match: {m: 'a'}}]);
runDoesntRewriteTest({t: 1}, null, {m: 1, t: 1}, ciStringColl, [{$match: {m: 'a'}}]);
