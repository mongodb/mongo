/**
 * Regression test for an invariant failure (server crash) when running an update or delete on a
 * time-series collection with a query predicate of the form {$jsonSchema: <non-object>}.
 *
 * The time-series write path rewrites the query (to rename the metaField) before it is parsed into
 * a match expression. That rewrite special-cased $jsonSchema and assumed its argument was an
 * object, tripping an invariant for a non-object value (e.g. an array). A malformed $jsonSchema
 * argument should instead be rejected with a TypeMismatch error, matching the behavior on a regular
 * (non-time-series) collection.
 *
 * @tags: [
 *   # We need a time-series collection.
 *   requires_timeseries,
 *   # Uses multi:true updates.
 *   requires_multi_updates,
 *   # This test depends on writes targeting buckets by metaField; stepdowns may split writes.
 *   does_not_support_stepdowns,
 *   # The "valid $jsonSchema" update case requires arbitrary time-series updates to be enabled.
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

(function() {
"use strict";

// In mongos-based passthroughs where sharded time-series updates/deletes are disabled, the final
// "valid $jsonSchema" update case would fail with InvalidOptions rather than succeeding — a
// spurious failure unrelated to $jsonSchema validation. Skip to stay configuration-independent.
if (FixtureHelpers.isMongos(db) && !TimeseriesTest.arbitraryUpdatesEnabled(db)) {
    jsTestLog("Skipping test: sharded time-series updates are not enabled in this configuration.");
    quit();
}

const timeFieldName = "t";
const metaFieldName = "m";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.getCollection("ts");

// Non-object $jsonSchema arguments that previously tripped an invariant during the time-series
// query rewrite.
const nonObjectSchemas = [[], "string", 1, true];

for (const schema of nonObjectSchemas) {
    coll.drop();
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }));
    assert.commandWorked(coll.insert({[timeFieldName]: new Date(), [metaFieldName]: 1, x: 1}));

    assert.commandFailedWithCode(
        testDB.runCommand({
            update: coll.getName(),
            updates: [{q: {$jsonSchema: schema}, u: {$set: {[metaFieldName]: 2}}, multi: true}],
        }),
        ErrorCodes.TypeMismatch,
        `expected update with $jsonSchema: ${tojson(schema)} to fail with TypeMismatch`);

    assert.commandFailedWithCode(
        testDB.runCommand({
            delete: coll.getName(),
            deletes: [{q: {$jsonSchema: schema}, limit: 0}],
        }),
        ErrorCodes.TypeMismatch,
        `expected delete with $jsonSchema: ${tojson(schema)} to fail with TypeMismatch`);
}

// A valid object $jsonSchema predicate on the metaField should still work.
coll.drop();
assert.commandWorked(testDB.createCollection(coll.getName(), {
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
}));
assert.commandWorked(coll.insert({[timeFieldName]: new Date(), [metaFieldName]: 1, x: 1}));
assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [{
        q: {$jsonSchema: {required: [metaFieldName]}},
        u: {$set: {[metaFieldName]: 2}},
        multi: true,
    }],
}));
assert.eq(1, coll.find({[metaFieldName]: 2}).itcount());
}());
