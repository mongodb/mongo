/**
 * Tests basic create and drop timeseries Collection behavior.
 *
 * @tags: [
 *   # "Overriding safe failed response for :: create"
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    areViewlessTimeseriesEnabled
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const viewlessTimeseriesEnabled = areViewlessTimeseriesEnabled(db);
const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = 'time';
const collName = 'ts';
const coll = testDB[collName];
const bucketsName = TimeseriesTest.getBucketsCollName(collName);

function assertCollExists(
    exists,
    db,
    collName,
) {
    let collInfo = db.getCollection(collName).getMetadata();
    if (exists) {
        assert(collInfo, `Collection '${collName}' was not found`);
    } else {
        assert(!collInfo,
               `Collection '${collName}' should not exists, but it was found: ${tojson(collInfo)}`);
    }
}

// Create a timeseries collection
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeFieldName}}));
assertCollExists(true, testDB, collName);
if (viewlessTimeseriesEnabled) {
    assertCollExists(false, testDB, bucketsName);
} else {
    assertCollExists(true, testDB, bucketsName);  // listCollection should show bucket collection
}

// Drop timeseries collection
coll.drop();
assertCollExists(false, testDB, collName);
assertCollExists(false, testDB, bucketsName);  // Bucket collection should also have been dropped

// Create a regular collection on the same namespace and verify result
assert.commandWorked(testDB.createCollection(collName));
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);

// Create timeseries collection when regular collection already exist on namespace. Command should
// fail with NamespaceExists
assert.commandFailedWithCode(
    testDB.createCollection(collName, {timeseries: {timeField: timeFieldName}}),
    ErrorCodes.NamespaceExists);
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);  // Validate that no orphaned bucket collection exists

coll.drop();
assertCollExists(false, testDB, collName);
assertCollExists(false, testDB, bucketsName);

// Create a view on the same namespace and verify result
testDB.getCollection("other");
assert.commandWorked(
    testDB.runCommand({create: collName, viewOn: "other", pipeline: [{$match: {field: "A"}}]}));
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);

// Create timeseries collection when view already exist on namespace. Command should fail with
// NamespaceExists
assert.commandFailedWithCode(
    testDB.createCollection(collName, {timeseries: {timeField: timeFieldName}}),
    ErrorCodes.NamespaceExists);
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);  // Validate that no orphaned bucket collection exists
