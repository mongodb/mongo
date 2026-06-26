/**
 * Tests that on a legacy (view-based) sharded timeseries collection, `fixedBucketing` remains absent from both
 * `listCollections` and the global catalog (`config.collections.timeseriesFields`) before and after a
 * bucketing-changing `collMod` (i.e., the fixedBucketing auto-disable logic for viewless timeseries must not be active
 * for legacy timeseries).
 *
 * TODO(SERVER-120014): remove this test once 9.0 becomes last LTS and all timeseries collections are viewless.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Avoid auto-sharding: this test performs explicit calls to shardCollection
 *   assumes_unsharded_collection,
 * ]
 */
import {
    findTimeseriesConfigCollectionsDocument,
    skipTestIfViewlessTimeseriesEnabled,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

skipTestIfViewlessTimeseriesEnabled(db);

const mongos = db.getMongo();
const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";
const collNss = `${testDB.getName()}.${collName}`;

assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(mongos.adminCommand({enableSharding: testDB.getName()}));

// Reads fixedBucketing for the test collection via listCollections.
function getListCollFixedBucketing() {
    const colls = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}}),
    ).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection", {colls});
    return colls[0].options.timeseries.fixedBucketing;
}

// Reads fixedBucketing from config.collections for the test collection (undefined when absent).
function getConfigFixedBucketing() {
    const entry = findTimeseriesConfigCollectionsDocument(testDB[collName]);
    assert(entry, "expected config.collections entry for sharded collection", {collNss});
    return entry.timeseriesFields.fixedBucketing;
}

// Shard a legacy timeseries collection (no fixedBucketing — the field is only valid on viewless).
assert.commandWorked(
    mongos.adminCommand({
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

// A collMod that changes bucketing parameters must not set fixedBucketing in either catalog.
assert.commandWorked(
    testDB.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: 200, bucketRoundingSeconds: 200},
    }),
);
assert.eq(getConfigFixedBucketing(), undefined);
assert.eq(getListCollFixedBucketing(), undefined);

assert.commandWorked(testDB.dropDatabase());
