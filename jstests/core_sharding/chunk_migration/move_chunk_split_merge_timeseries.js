/**
 * Test split, moveChunk and mergeChunks commands on timeseries collections.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_2_or_more_shards,
 *   # TODO SERVER-107141 re-enable this test in stepdown suites
 *   does_not_support_stepdowns,
 *   assumes_balancer_off,
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRandomShardName} from "jstests/libs/sharded_cluster_fixture_helpers.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const coll = db[jsTestName()];
const timeField = "t";
const metaField = "m";

function numChunksOnShard(shardId) {
    const configDB = db.getSiblingDB("config");
    return findChunksUtil.countChunksForNs(configDB, getTimeseriesCollForDDLOps(db, coll).getFullName(), {
        shard: shardId,
    });
}

assert.commandWorked(db.runCommand({drop: coll.getName()}));

assert.commandWorked(
    db.adminCommand({
        shardCollection: coll.getFullName(),
        key: {[metaField]: 1},
        timeseries: {timeField: timeField, metaField: metaField},
    }),
);

const primaryShardId = coll.getDB().getDatabasePrimaryShardId();
const otherShardId = getRandomShardName(db, [primaryShardId]);

const allDocs = [
    {[timeField]: new ISODate(), [metaField]: "a"},
    {[timeField]: new ISODate(), [metaField]: "b"},
];
assert.commandWorked(coll.insertMany(allDocs));

// Split at meta "b".
assert.commandWorked(
    db.adminCommand({
        split: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        middle: {meta: "b"},
    }),
);
assert.eq(2, numChunksOnShard(primaryShardId));
assert.eq(0, numChunksOnShard(otherShardId));
assert.sameMembers(allDocs, coll.find({}, {_id: 0}).toArray());

// moveChunk: move the "b" chunk to otherShard.
assert.commandWorked(
    db.adminCommand({
        moveChunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        find: {meta: "b"},
        to: otherShardId,
    }),
);
assert.eq(1, numChunksOnShard(primaryShardId));
assert.eq(1, numChunksOnShard(otherShardId));
assert.sameMembers(allDocs, coll.find({}, {_id: 0}).toArray());

// Move it back so we can merge.
assert.commandWorked(
    db.adminCommand({
        moveChunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        find: {meta: "b"},
        to: primaryShardId,
    }),
);
assert.eq(2, numChunksOnShard(primaryShardId));
assert.eq(0, numChunksOnShard(otherShardId));
assert.sameMembers(allDocs, coll.find({}, {_id: 0}).toArray());

// Merge all chunks back together.
assert.commandWorked(
    db.adminCommand({
        mergeChunks: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        bounds: [{meta: MinKey}, {meta: MaxKey}],
    }),
);
assert.eq(1, numChunksOnShard(primaryShardId));
assert.eq(0, numChunksOnShard(otherShardId));
assert.sameMembers(allDocs, coll.find({}, {_id: 0}).toArray());

// TODO(SERVER-121914): Remove this once orphans cleanup can not get stuck
coll.drop();
