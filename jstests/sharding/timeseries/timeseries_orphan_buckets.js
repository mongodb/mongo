/**
 * Tests to validate that an orphan bucket is not updatable after a chunk migration.
 *
 * @tags: [
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

const dbName = "test";
const collName = "foo";
const timeField = "time";
const metaField = "hostid";

const st = new ShardingTest({shards: 2});
const sDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const primaryShard = st.getPrimaryShard(dbName);
const otherShard = st.getOther(primaryShard);

let currentId = 0;
function generateId() {
    return currentId++;
}

function generateDoc(time, metaValue) {
    return TimeseriesTest.generateHosts(1).map((host, index) =>
        Object.assign(host, {
            _id: generateId(),
            [metaField]: metaValue,
            [timeField]: ISODate(time),
        }),
    )[0];
}

const shardKey = {
    [timeField]: 1,
};
assert.commandWorked(
    sDB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField, granularity: "hours"}}),
);
assert.commandWorked(
    sDB.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
    }),
);

const coll = sDB.getCollection(collName);

// Split the chunks such that primary shard has chunk: [MinKey, 2020-01-01) and other shard has
// chunk [2020-01-01, MaxKey].
let splitPoint = {[`control.min.${timeField}`]: ISODate(`2020-01-01`)};
assert.commandWorked(
    sDB.adminCommand({split: getTimeseriesCollForDDLOps(sDB, coll).getFullName(), middle: splitPoint}),
);

let counts = st.chunkCounts(collName, dbName);
assert.eq(2, counts[primaryShard.shardName], counts);
assert.eq(0, counts[otherShard.shardName], counts);

for (let i = 0; i < 2; i++) {
    assert.commandWorked(coll.insert(generateDoc("2019-12-24", i)));
    assert.commandWorked(coll.insert(generateDoc("2019-12-29", i)));
    assert.commandWorked(coll.insert(generateDoc("2020-01-02", i)));
    assert.commandWorked(coll.insert(generateDoc("2020-01-04", i)));
}

jsTest.log("Assert that there are no range deletion tasks");
assert.eq(0, primaryShard.getDB("config").getCollection("rangeDeletions").count());
assert.eq(0, otherShard.getDB("config").getCollection("rangeDeletions").count());

let suspendRangeDeletionShard0 = configureFailPoint(primaryShard, "suspendRangeDeletion");

assert.commandWorked(
    sDB.adminCommand({
        movechunk: getTimeseriesCollForDDLOps(sDB, coll).getFullName(),
        find: {[`control.min.${timeField}`]: MinKey},
        to: otherShard.name,
        _waitForDelete: false,
    }),
);

// Ensure that each shard owns one chunk.
counts = st.chunkCounts(collName, dbName);
assert.eq(1, counts[primaryShard.shardName], counts);
assert.eq(1, counts[otherShard.shardName], counts);

assert.eq(8, coll.find().itcount());

for (let i = 0; i < 2; i++) {
    assert.commandWorked(coll.insert(generateDoc("2019-12-24", i)));
    assert.commandWorked(coll.insert(generateDoc("2019-12-29", i)));
    assert.commandWorked(coll.insert(generateDoc("2020-01-02", i)));
    assert.commandWorked(coll.insert(generateDoc("2020-01-04", i)));
}

assert.eq(16, coll.find().itcount(), coll.find().toArray());

assert.eq(0, otherShard.getDB("config").getCollection("rangeDeletions").count());
assert.eq(1, primaryShard.getDB("config").getCollection("rangeDeletions").count());

suspendRangeDeletionShard0.off();
const res = primaryShard.adminCommand({
    cleanupOrphaned: getTimeseriesCollForDDLOps(sDB, coll).getFullName(),
    startingFromKey: {
        [`control.min.${timeField}`]: MinKey,
    } /* The startingFromKey parameter should be ignored
     */,
});
assert.commandWorked(res);
assert.eq(0, primaryShard.getDB("config").getCollection("rangeDeletions").count());
assert.eq(16, coll.find().itcount(), coll.find().toArray());

st.stop();
