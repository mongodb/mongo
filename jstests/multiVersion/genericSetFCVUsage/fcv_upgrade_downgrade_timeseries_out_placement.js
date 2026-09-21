/**
 * Verifies that $out preserves the placement of a tracked time-series collection before and after
 * downgrading from the viewless format to the legacy format.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagMoveCollection,
 *   assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const kDbName = jsTestName();
const kSourceCollName = "source";
const kDestCollName = "destination";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
const db = st.s.getDB(kDbName);
const primaryShard = st.shard0;
const destinationShard = st.shard1;

assert.commandWorked(
    db.adminCommand({enableSharding: kDbName, primaryShard: primaryShard.shardName}),
);
assert.commandWorked(db[kSourceCollName].insertMany([{x: 1}, {x: 2}, {x: 3}]));

function assertPlacement(collName, expectedBucketCollection) {
    const physicalCollName = expectedBucketCollection
        ? getTimeseriesBucketsColl(collName)
        : collName;
    const destinationCount = destinationShard
        .getDB(kDbName)
        .getCollection(physicalCollName)
        .countDocuments({});
    const primaryCount = primaryShard
        .getDB(kDbName)
        .getCollection(physicalCollName)
        .countDocuments({});

    assert.gt(destinationCount, 0, {collName, physicalCollName, destinationCount});
    assert.eq(primaryCount, 0, {collName, physicalCollName, primaryCount});
}

function assertOutPreservesPlacement(expectedBucketCollection) {
    const coll = db[kDestCollName];
    db[kSourceCollName]
        .aggregate([{$set: {t: ISODate("2020-01-01T00:00:00Z")}}, {$out: coll.getName()}])
        .toArray();
    assert.eq(3, coll.countDocuments({}));
    assertPlacement(kDestCollName, expectedBucketCollection);
}

assert.commandWorked(db.createCollection(kDestCollName, {timeseries: {timeField: "t"}}));
assert.commandWorked(
    db[kDestCollName].insertMany([
        {t: ISODate("2026-01-01T00:00:00Z"), x: 1},
        {t: ISODate("2026-01-02T00:00:00Z"), x: 2},
        {t: ISODate("2026-01-03T00:00:00Z"), x: 3},
    ]),
);

// move collection to shard 1 (destination shard)
assert.commandWorked(
    st.s.adminCommand({
        moveCollection: `${kDbName}.${kDestCollName}`,
        toShard: destinationShard.shardName,
    }),
);

assertPlacement(kDestCollName, false);
assertOutPreservesPlacement(false);

assert.eq(true, areViewlessTimeseriesEnabled(db));

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert.eq(false, areViewlessTimeseriesEnabled(db));
assertPlacement(kDestCollName, true);
assertOutPreservesPlacement(true);

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
st.stop();
