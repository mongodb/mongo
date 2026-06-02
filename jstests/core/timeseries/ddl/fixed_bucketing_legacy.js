/**
 * Tests that on a legacy (view-based) timeseries collection, a `collMod` that changes bucketing
 * parameters does not set `fixedBucketing` in the catalog. `fixedBucketing` is only valid for
 * viewless timeseries collections; this test ensures the auto-disable logic does not leak the
 * field into legacy catalog entries.
 *
 * TODO(SERVER-126823): remove this test once 9.0 becomes last LTS and all timeseries collections
 * are viewless.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */
import {skipTestIfViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

skipTestIfViewlessTimeseriesEnabled(db);

const collName = jsTestName();
const coll = db.getCollection(collName);

coll.drop();
assert.commandWorked(
    db.createCollection(collName, {
        timeseries: {timeField: "t", bucketMaxSpanSeconds: 100, bucketRoundingSeconds: 100},
    }),
);

// Reads the `fixedBucketing` value visible via `listCollections` for the test collection.
function getStoredFixedBucketing() {
    const colls = db.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}}).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection", {colls});
    return colls[0].options.timeseries.fixedBucketing;
}

// The field must be absent on a freshly-created legacy collection.
assert.eq(getStoredFixedBucketing(), undefined);

// A collMod that changes the bucketing parameters must not set `fixedBucketing` in the catalog.
assert.commandWorked(
    db.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: 200, bucketRoundingSeconds: 200},
    }),
);
assert.eq(getStoredFixedBucketing(), undefined);
