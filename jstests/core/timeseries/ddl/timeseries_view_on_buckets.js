/**
 * Tests that creating or modifying a view whose viewOn targets a system.buckets collection is
 * rejected when viewless timeseries are enabled.
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

const testDB = db.getSiblingDB(jsTestName());

const tsCollName = "foo";
const bucketsCollName = "system.buckets." + tsCollName;

const newViewName = "newView";
assert.commandWorked(testDB.runCommand({drop: newViewName}));
assert.commandFailedWithCode(testDB.createView(newViewName, bucketsCollName, []), ErrorCodes.InvalidNamespace);

const existingViewName = "existingView";
assert.commandWorked(testDB.runCommand({drop: existingViewName}));
assert.commandWorked(testDB.createView(existingViewName, "otherColl", []));
assert.commandFailedWithCode(
    testDB.runCommand({collMod: existingViewName, viewOn: bucketsCollName, pipeline: []}),
    ErrorCodes.InvalidNamespace,
);
