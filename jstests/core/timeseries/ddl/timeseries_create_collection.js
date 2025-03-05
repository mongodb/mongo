/**
 * Tests basic create and drop timeseries Collection behavior. Also test that we fail with
 * NamespaceExists when namespace is already used and that we don't leave orphaned bucket collection
 * in that case.
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
    let collections =
        new DBCommandCursor(
            db, assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}})))
            .toArray();
    if (exists) {
        assert.eq(1,
                  collections.length,
                  `Collection '${collName}' was not found through listCollections command`);
    } else {
        assert.eq(0,
                  collections.length,
                  `Collection '${
                      collName}' should not exists, but it was found through listCollections: ${
                      tojson(collections)}`);
    }
}

// Create a timeseries collection, listCollection should show view and bucket collection
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeFieldName}}));
assertCollExists(true, testDB, collName);
if (viewlessTimeseriesEnabled) {
    assertCollExists(false, testDB, bucketsName);
} else {
    assertCollExists(true, testDB, bucketsName);
}

// Drop timeseries collection, both view and bucket collection should be dropped
coll.drop();
assertCollExists(false, testDB, collName);
assertCollExists(false, testDB, bucketsName);

// Create a regular collection on the same namespace and verify result
assert.commandWorked(testDB.createCollection(collName));
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);

// Create timeseries collection when regular collection already exist on namespace. Command should
// fail with NamespaceExists and no bucket collection should be created
assert.commandFailedWithCode(
    testDB.createCollection(collName, {timeseries: {timeField: timeFieldName}}),
    ErrorCodes.NamespaceExists);
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);

coll.drop();
assertCollExists(false, testDB, collName);
assertCollExists(false, testDB, bucketsName);

// Create a regular view on the same namespace and verify result
testDB.getCollection("other");
assert.commandWorked(
    testDB.runCommand({create: collName, viewOn: "other", pipeline: [{$match: {field: "A"}}]}));
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);

// Create timeseries collection when view already exist on namespace. Command should fail with
// NamespaceExists and no bucket collection should be created
assert.commandFailedWithCode(
    testDB.createCollection(collName, {timeseries: {timeField: timeFieldName}}),
    ErrorCodes.NamespaceExists);
assertCollExists(true, testDB, collName);
assertCollExists(false, testDB, bucketsName);
