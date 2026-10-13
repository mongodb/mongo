/**
 * Verifies that $out preserves the placement of a legacy time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagMoveCollection,
 *   assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const kDbName = jsTestName();
const kSourceCollName = "source";
const kDestCollName = "destination";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
const db = st.s.getDB(kDbName);
const primaryShard = st.shard0;
const destinationShard = st.shard1;

assert.commandWorked(db.adminCommand({enableSharding: kDbName, primaryShard: primaryShard.shardName}));
assert.commandWorked(
    db[kSourceCollName].insertMany([
        {t: ISODate("2026-01-01T00:00:00Z"), x: 1},
        {t: ISODate("2026-01-02T00:00:00Z"), x: 2},
        {t: ISODate("2026-01-03T00:00:00Z"), x: 3},
    ]),
);
assert.commandWorked(db.createCollection(kDestCollName, {timeseries: {timeField: "t"}}));
assert.commandWorked(
    db[kDestCollName].insertMany([
        {t: ISODate("2026-01-01T00:00:00Z"), x: 1},
        {t: ISODate("2026-01-02T00:00:00Z"), x: 2},
        {t: ISODate("2026-01-03T00:00:00Z"), x: 3},
    ]),
);

const bucketsCollName = getTimeseriesBucketsColl(kDestCollName);
assert.commandWorked(
    st.s.adminCommand({
        moveCollection: `${kDbName}.${kDestCollName}`,
        toShard: destinationShard.shardName,
    }),
);

function assertPlacement() {
    const destinationCount = destinationShard.getDB(kDbName).getCollection(bucketsCollName).countDocuments({});
    const primaryCount = primaryShard.getDB(kDbName).getCollection(bucketsCollName).countDocuments({});

    assert.gt(destinationCount, 0, {bucketsCollName, destinationCount});
    assert.eq(primaryCount, 0, {bucketsCollName, primaryCount});
}

assertPlacement();
db[kSourceCollName].aggregate([{$out: kDestCollName}]).toArray();
assert.eq(3, db[kDestCollName].countDocuments({}));
assertPlacement();

st.stop();
