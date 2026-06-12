/**
 * Tests that on a legacy (view-based) sharded timeseries collection, a `collMod` that changes
 * bucketing parameters does not set `fixedBucketing` in the global catalog
 * (`config.collections.timeseriesFields`) or in the durable catalog (visible via
 * `listCollections`). `fixedBucketing` is only valid for viewless timeseries collections; this
 * test ensures the auto-disable logic does not leak the field into legacy catalog entries.
 *
 * TODO(SERVER-126823): remove this test once 9.0 becomes last LTS and all timeseries collections
 * are viewless.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */
import {skipTestIfViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});

const dbName = "test";
const collName = "ts";
const collNss = `${dbName}.${collName}`;
const sDB = st.s.getDB(dbName);

skipTestIfViewlessTimeseriesEnabled(sDB, () => st.stop());

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Reads the `fixedBucketing` value persisted in `config.collections` for the buckets namespace.
function getConfigFixedBucketing() {
    return st.s.getDB("config").collections.findOne({_id: `${dbName}.system.buckets.${collName}`})
        .timeseriesFields.fixedBucketing;
}

// Reads the `fixedBucketing` value visible via `listCollections` for the test collection.
function getListCollFixedBucketing() {
    const colls = sDB.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}})
        .cursor.firstBatch;
    assert.eq(colls.length, 1, colls);
    return colls[0].options.timeseries.fixedBucketing;
}

// Shard a legacy timeseries collection (no fixedBucketing — the field is only valid on viewless).
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: collNss,
        key: {"hostId": 1},
        timeseries: {
            timeField: "time",
            metaField: "hostId",
            bucketMaxSpanSeconds: 100,
            bucketRoundingSeconds: 100,
        },
    }),
);
assert.eq(getConfigFixedBucketing(), undefined);
assert.eq(getListCollFixedBucketing(), undefined);

// A collMod that changes the bucketing parameters must not set `fixedBucketing` in either catalog.
assert.commandWorked(
    sDB.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: 200, bucketRoundingSeconds: 200},
    }),
);
assert.eq(getConfigFixedBucketing(), undefined);
assert.eq(getListCollFixedBucketing(), undefined);

st.stop();
