/*
 * Tests basic movePrimary behavior with timeseries collections.
 *
 * @tags: [
 *  requires_timeseries,
 *  # movePrimary command is not allowed in clusters with a single shard.
 *  requires_2_or_more_shards,
 *  # movePrimary will fail if the destination shard steps down while cloning data.
 *  does_not_support_stepdowns,
 *  # This test performs explicit calls to shardCollection
 *  assumes_unsharded_collection,
 *  # Expects databases to be in specific locations
 *  assumes_stable_shard_list,
 * ]
 */

import {getRandomShardName} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const coll = db[jsTestName()];

const N = 1500;

function doInserts(n) {
    jsTestLog("Inserting " + n + " measurements.");

    const docs = [];
    for (let i = 0; i < n; i++) {
        docs.push({t: ISODate(), m: {sensorId: i % 20}, temp: i / 42});
    }
    coll.insertMany(docs);
}

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

const initPrimaryShard = db.getDatabasePrimaryShardId();

// Create a timeseries collection
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
assert.commandWorked(coll.createIndex({temp: 1, t: 1}));
doInserts(N);
assert.eq(N, coll.countDocuments({}));

const otherShard = getRandomShardName(db, /* exclude = */ initPrimaryShard);

jsTestLog("Move primary to another shard and check content.");
assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: otherShard}));
assert.eq(otherShard, db.getDatabasePrimaryShardId());
assert.eq(N, coll.countDocuments({}));
doInserts(N);
assert.eq(2 * N, coll.countDocuments({}));

jsTestLog("Move primary to the original shard and check content.");
assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: initPrimaryShard}));
assert.eq(initPrimaryShard, db.getDatabasePrimaryShardId());
assert.eq(2 * N, coll.countDocuments({}));
doInserts(N);
assert.eq(3 * N, coll.countDocuments({}));

// Run some sanity checks on the metadata
assert.eq("timeseries", coll.getMetadata().type);
assert.sameMembers(
    [
        {m: 1, t: 1},
        {temp: 1, t: 1},
    ],
    coll.getIndexKeys(),
);
