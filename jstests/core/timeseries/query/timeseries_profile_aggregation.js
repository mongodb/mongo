/**
 * Validates the profiler entry for a timeseries aggregation.
 *
 * @tags: [
 *   requires_timeseries,
 *   # The test runs commands that are not allowed with security token: setProfilingLevel.
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   requires_fcv_83,
 *   requires_profiling,
 *  # The test runs getLatestProfileEntry(). The downstream syncing node affects the profiler.
 *   run_getLatestProfilerEntry,
 * ]
 */

import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const dbName = "test_db";
const tsCollName = "test_ts_coll";
const testDB = db.getSiblingDB(dbName);
const tsColl = testDB.getCollection(tsCollName);

assert.commandWorked(
    testDB.setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

tsColl.drop();

assert.commandWorked(
    testDB.createCollection(tsCollName, {
        timeseries: {
            timeField: "t",
            metaField: "m",
        },
    }),
);

// Insert mock time-series data into the collection.
const now = new Date();
assert.commandWorked(
    tsColl.insertMany([
        {t: new Date(now - 1000), m: "a", val: 1},
        {t: new Date(now), m: "a", val: 2},
        {t: new Date(now + 1000), m: "a", val: 3},
    ]),
);

// Comment on the timeseries aggregation, with a uuid to uniquely identify the command.
const commentObj = {
    uuid: UUID().hex(),
};

const results = tsColl.aggregate([{$match: {val: {$gt: 0}}}], {"comment": commentObj}).toArray();
assert.eq(results.length, 3);

let profileObj = getLatestProfilerEntry(testDB);

// Validate profiler entry structure for timeseries aggregation.
assert.eq(profileObj.op, "command");
assert.eq(profileObj.ns, tsColl.getFullName());
assert.eq(profileObj.command.aggregate, tsCollName);
assert.eq(profileObj.command.comment, commentObj);
assert.eq(profileObj.docsExamined, 2);

if (!areViewlessTimeseriesEnabled(db)) {
    // For view-ful timeseries, there is extra info about the resolved view.
    assert.eq(profileObj.resolvedViews.length, 1);
    let tsResolvedViewObj = profileObj.resolvedViews[0];

    assert.eq(tsResolvedViewObj.viewNamespace, tsColl.getFullName());
    assert.eq(tsResolvedViewObj.dependencyChain, [tsCollName, getTimeseriesBucketsColl(tsCollName)]);
}
