/**
 * Tests that creating or modifying a view whose viewOn targets a system.buckets collection is
 * allowed when viewless timeseries are not enabled (legacy viewful timeseries).
 *
 * TODO SERVER-120014: remove this test once 9.0 becomes last LTS and all timeseries collections are viewless.
 *
 * @tags: [
 *   # A retry of collMod after a stepdown may fail since it interprets the view as a legacy timeseries view.
 *   does_not_support_stepdowns,
 *   # TODO(SERVER-125060): Remove this tag.
 *   requires_fcv_81,
 * ]
 */

import {skipTestIfViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

skipTestIfViewlessTimeseriesEnabled(db);

const testDB = db.getSiblingDB(jsTestName());

const tsCollName = "foo";
const bucketsCollName = "system.buckets." + tsCollName;
assert.commandWorked(testDB.runCommand({drop: tsCollName}));
assert.commandWorked(testDB.createCollection(tsCollName, {timeseries: {timeField: "t"}}));

// Create a new timeseries view over a buckets collection
const newViewName = "newView";
assert.commandWorked(testDB.runCommand({drop: newViewName}));
assert.commandWorked(testDB.createView(newViewName, bucketsCollName, []));

// collMod a view to point to a buckets collection
const existingViewName = "existingView";
assert.commandWorked(testDB.runCommand({drop: existingViewName}));
assert.commandWorked(testDB.createView(existingViewName, "otherColl", []));
assert.commandWorked(testDB.runCommand({collMod: existingViewName, viewOn: bucketsCollName, pipeline: []}));

// Clean up the inconsistent timeseries views to avoid them causing validation failures
assert.commandWorked(testDB.runCommand({drop: existingViewName}));
assert.commandWorked(testDB.runCommand({drop: newViewName}));
