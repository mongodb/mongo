/**
 * Tests that on a legacy (view-based) timeseries collection:
 *   1. Creating a collection without `fixedBucketing` does not set the field in the catalog
 *      (the default-true logic must not apply to legacy collections).
 *   2. Idempotently re-creating the collection while omitting `fixedBucketing` succeeds and
 *      leaves the field absent.
 *   3. A `collMod` that changes bucketing parameters does not set `fixedBucketing` in the catalog.
 *   4. Attempting to create a legacy collection with `fixedBucketing: true` is rejected.
 *
 * `fixedBucketing` is only valid for viewless timeseries collections.
 *
 * TODO(SERVER-120014): remove this test once 9.0 becomes last LTS and all timeseries collections
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

// Reads the `fixedBucketing` value visible via `listCollections` for the test collection.
function getStoredFixedBucketing() {
    const colls = db.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}})
        .cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection", {colls});
    return colls[0].options.timeseries.fixedBucketing;
}

// Case 1: the field must be absent on a freshly-created legacy collection. The default-true logic
// introduced in SERVER-126823 must not apply to legacy (view-based) collections.
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {
        timeseries: {timeField: "t", bucketMaxSpanSeconds: 100, bucketRoundingSeconds: 100},
    }),
);
assert.eq(getStoredFixedBucketing(), undefined);

// Case 2: re-creating an existing legacy collection without specifying `fixedBucketing` is
// idempotent — it succeeds and leaves the field absent.
assert.commandWorked(
    db.createCollection(collName, {
        timeseries: {timeField: "t", bucketMaxSpanSeconds: 100, bucketRoundingSeconds: 100},
    }),
);
assert.eq(getStoredFixedBucketing(), undefined);

// Case 3: a collMod that changes the bucketing parameters must not set `fixedBucketing` in the
// catalog.
assert.commandWorked(
    db.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: 200, bucketRoundingSeconds: 200},
    }),
);
assert.eq(getStoredFixedBucketing(), undefined);

// Case 4: attempting to create a legacy timeseries collection with `fixedBucketing` set (true or
// false) must be rejected. The field is only valid on viewless timeseries collections.
coll.drop();
const rejectedCodes = [
    ErrorCodes.InvalidOptions,
    // TODO(SERVER-120014): Remove IDLUnknownField once 9.0 becomes last LTS. On pre-9.0 binaries
    // `fixedBucketing` is an unknown field and is rejected by the IDL parser.
    ErrorCodes.IDLUnknownField,
];
assert.commandFailedWithCode(
    db.createCollection(collName, {timeseries: {timeField: "t", fixedBucketing: true}}),
    rejectedCodes,
);
assert.commandFailedWithCode(
    db.createCollection(collName, {timeseries: {timeField: "t", fixedBucketing: false}}),
    rejectedCodes,
);
