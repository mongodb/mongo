/**
 * Tests that serverStatus contains an indexStats section. This section reports globally-aggregated
 * statistics about features in use by indexes and how often they are used.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {
    assertCountIncrease,
    assertFeatureAccessIncrease,
    assertFeatureCountIncrease,
    assertStats,
    assertZeroAccess,
    assertZeroCounts,
} from "jstests/libs/index_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let db = primary.getDB("test");

assertZeroCounts(db);
assertZeroAccess(db);

let lastStats = db.serverStatus().indexStats;

assert.commandWorked(db.testColl.createIndex({twoD: "2d", b: 1}, {unique: true, sparse: true}));
assert.commandWorked(db.testColl.insert({twoD: [0, 0], b: 1}));
assert.eq(1, db.testColl.find({twoD: {$geoNear: [0, 0]}}).itcount());
assertStats(db, (stats) => {
    assertCountIncrease(lastStats, stats, 2);
    assertFeatureCountIncrease(lastStats, stats, "2d", 1);
    assertFeatureCountIncrease(lastStats, stats, "compound", 1);
    // The index build implicitly created the collection, which also builds an _id index.
    assertFeatureCountIncrease(lastStats, stats, "id", 1);
    assertFeatureCountIncrease(lastStats, stats, "sparse", 1);
    // Note that the _id index is not included in this unique counter. This is due to a quirk in the
    // _id index spec that does not actually have a unique:true property.
    assertFeatureCountIncrease(lastStats, stats, "unique", 1);

    assertFeatureAccessIncrease(lastStats, stats, "2d", 1);
    assertFeatureAccessIncrease(lastStats, stats, "compound", 1);
    assertFeatureAccessIncrease(lastStats, stats, "id", 0);
    assertFeatureAccessIncrease(lastStats, stats, "sparse", 1);
    assertFeatureAccessIncrease(lastStats, stats, "unique", 1);
});

lastStats = db.serverStatus().indexStats;

assert.commandWorked(db.testColl.createIndex({sphere: "2dsphere"}));
assert.commandWorked(db.testColl.insert({sphere: {type: "Point", coordinates: [0, 0]}}));
assert.eq(
    1,
    db.testColl
        .aggregate([
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [1, 1]},
                    key: "sphere",
                    distanceField: "dist",
                },
            },
        ])
        .itcount(),
);
assertStats(db, (stats) => {
    assertCountIncrease(lastStats, stats, 1);
    assertFeatureCountIncrease(lastStats, stats, "2dsphere", 1);
    assertFeatureCountIncrease(lastStats, stats, "single", 1);

    assertFeatureAccessIncrease(lastStats, stats, "2dsphere", 1);
    assertFeatureAccessIncrease(lastStats, stats, "single", 1);
});

lastStats = db.serverStatus().indexStats;

assert.commandWorked(db.testColl.createIndex({hashed: "hashed", p: 1}, {partialFilterExpression: {p: 1}}));
assert.commandWorked(db.testColl.insert({hashed: 1, p: 1}));
assert.eq(1, db.testColl.find({hashed: 1}).hint({hashed: "hashed", p: 1}).itcount());
assertStats(db, (stats) => {
    assertCountIncrease(lastStats, stats, 1);
    assertFeatureCountIncrease(lastStats, stats, "compound", 1);
    assertFeatureCountIncrease(lastStats, stats, "hashed", 1);
    assertFeatureCountIncrease(lastStats, stats, "partial", 1);

    assertFeatureAccessIncrease(lastStats, stats, "compound", 1);
    assertFeatureAccessIncrease(lastStats, stats, "hashed", 1);
    assertFeatureAccessIncrease(lastStats, stats, "partial", 1);
});

lastStats = db.serverStatus().indexStats;

assert.commandWorked(db.testColl.createIndex({a: 1}, {expireAfterSeconds: 3600, collation: {locale: "en"}}));
let now = new Date();
assert.commandWorked(db.testColl.insert({a: now}));
assert.eq(1, db.testColl.find({a: now}).itcount());
assertStats(db, (stats) => {
    assertCountIncrease(lastStats, stats, 1);
    assertFeatureCountIncrease(lastStats, stats, "collation", 1);
    assertFeatureCountIncrease(lastStats, stats, "normal", 1);
    assertFeatureCountIncrease(lastStats, stats, "single", 1);
    assertFeatureCountIncrease(lastStats, stats, "ttl", 1);

    assertFeatureAccessIncrease(lastStats, stats, "collation", 1);
    assertFeatureAccessIncrease(lastStats, stats, "normal", 1);
    assertFeatureAccessIncrease(lastStats, stats, "single", 1);
    assertFeatureAccessIncrease(lastStats, stats, "ttl", 1);
});

lastStats = db.serverStatus().indexStats;

assert.commandWorked(db.testColl.createIndex({text: "text"}));
assert.commandWorked(db.testColl.insert({text: "a string"}));
assert.eq(1, db.testColl.find({$text: {$search: "string"}}).itcount());
assertStats(db, (stats) => {
    assertCountIncrease(lastStats, stats, 1);
    // Text indexes are internally compound, but that should not be reflected in the stats.
    assertFeatureCountIncrease(lastStats, stats, "compound", 0);
    assertFeatureCountIncrease(lastStats, stats, "text", 1);

    assertFeatureAccessIncrease(lastStats, stats, "compound", 0);
    assertFeatureAccessIncrease(lastStats, stats, "text", 1);
});

lastStats = db.serverStatus().indexStats;

assert.commandWorked(db.testColl.createIndex({"wild.$**": 1}));
assert.commandWorked(db.testColl.insert({wild: {a: 1}}));
assert.eq(1, db.testColl.find({"wild.a": 1}).itcount());
assertStats(db, (stats) => {
    assertCountIncrease(lastStats, stats, 1);
    assertFeatureCountIncrease(lastStats, stats, "single", 1);
    assertFeatureCountIncrease(lastStats, stats, "wildcard", 1);

    assertFeatureAccessIncrease(lastStats, stats, "single", 1);
    assertFeatureAccessIncrease(lastStats, stats, "wildcard", 1);
});

lastStats = db.serverStatus().indexStats;

assert.commandWorked(db.createCollection("ts", {timeseries: {timeField: "t"}}));
assert.commandWorked(db.ts.createIndex({loc: "2dsphere"}));
assert.commandWorked(db.ts.insert({t: new Date(), loc: [0, 0]}));
assert.eq(
    1,
    db.ts
        .aggregate(
            [
                {
                    $geoNear: {
                        near: [1, 1],
                        key: "loc",
                        distanceField: "dist",
                    },
                },
            ],
            {hint: "loc_2dsphere"},
        )
        .itcount(),
);
assertStats(db, (stats) => {
    let idIndexIncrement = 0;
    if (!areViewlessTimeseriesEnabled(db)) {
        // Includes _id index built for system.views.
        idIndexIncrement = 1;
    }
    assertCountIncrease(lastStats, stats, 1 + idIndexIncrement);
    assertFeatureCountIncrease(lastStats, stats, "id", idIndexIncrement);
    assertFeatureCountIncrease(lastStats, stats, "single", 1);
    assertFeatureCountIncrease(lastStats, stats, "2dsphere_bucket", 1);

    assertFeatureAccessIncrease(lastStats, stats, "id", 0);
    assertFeatureAccessIncrease(lastStats, stats, "single", 1);
    assertFeatureAccessIncrease(lastStats, stats, "2dsphere_bucket", 1);
});

lastStats = db.serverStatus().indexStats;

// After restarting the server, we expect all of the access counters to reset to zero, but that the
// feature counters remain the same as before startup.
replSet.stopSet(undefined, /* restart */ true);
replSet.startSet({}, /* restart */ true);
primary = replSet.getPrimary();
db = primary.getDB("test");

assertZeroAccess(db);
assertStats(db, (stats) => {
    assertCountIncrease(lastStats, stats, 0);

    const features = stats.features;
    for (const [feature, _] of Object.entries(features)) {
        assertFeatureCountIncrease(lastStats, stats, feature, 0);
    }
});

assert.commandWorked(db.dropDatabase());
assertZeroCounts(db);

replSet.stopSet();
