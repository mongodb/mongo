/**
 * Tests basic create and drop timeseries Collection behavior.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {isStableFCVSuite} from "jstests/libs/feature_compatibility_version.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = "tf";
const metaFieldName = "mf";
const collName = "ts";
const coll = testDB[collName];
const bucketsName = getTimeseriesBucketsColl(collName);
const otherName = "other";
const viewPipeline = [{$match: {field: "A"}}];

function assertCollExists(exists, db, collName) {
    let collInfo = db.getCollection(collName).getMetadata();
    if (exists) {
        assert(collInfo, `Collection '${collName}' was not found`);
    } else {
        assert(!collInfo, `Collection '${collName}' should not exists, but it was found: ${tojson(collInfo)}`);
    }
}

function dropMainNamespace() {
    coll.drop();
    assertCollExists(false, testDB, collName);
    assertCollExists(false, testDB, bucketsName);
    assertCollExists(false, testDB, otherName);
}
describe("Non existing collection", () => {
    afterEach(() => {
        dropMainNamespace();
    });
    it("Create non-timeseries coll", () => {
        assert.commandWorked(testDB.createCollection(collName));
        assertCollExists(true, testDB, collName);
        assertCollExists(false, testDB, bucketsName);
    });
    it("Create timeseries coll", () => {
        // Create a timeseries collection
        assert.commandWorked(
            testDB.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
        );
        assertCollExists(true, testDB, collName);
        if (isStableFCVSuite()) {
            if (areViewlessTimeseriesEnabled(db)) {
                assertCollExists(false, testDB, bucketsName);
            } else {
                assertCollExists(true, testDB, bucketsName); // listCollection should show bucket collection
            }
        }
    });
    it("Create view", () => {
        assert.commandWorked(testDB.runCommand({create: collName, viewOn: otherName, pipeline: viewPipeline}));
        assertCollExists(true, testDB, collName);
        assertCollExists(false, testDB, bucketsName);
        assertCollExists(false, testDB, otherName);
    });
});

describe("Non-timeseries coll exists", () => {
    beforeEach(() => {
        assert.commandWorked(testDB.createCollection(collName));
        assertCollExists(true, testDB, collName);
        assertCollExists(false, testDB, bucketsName);
    });
    afterEach(() => {
        assertCollExists(true, testDB, collName);
        assertCollExists(false, testDB, bucketsName);
        assertCollExists(false, testDB, otherName);
        dropMainNamespace();
    });
    it("Create non-timeseries coll", () => {
        // Creating a non-timeseries collection when the collection already exists should be idempotent
        assert.commandWorked(testDB.createCollection(collName));
    });
    it("Create timeseries coll", () => {
        // Create timeseries collection when regular collection already exist on namespace. Command should
        // fail with NamespaceExists
        assert.commandFailedWithCode(
            testDB.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
            ErrorCodes.NamespaceExists,
        );
    });
    it("Create view", () => {
        assert.commandFailedWithCode(
            testDB.runCommand({create: collName, viewOn: otherName, pipeline: viewPipeline}),
            ErrorCodes.NamespaceExists,
        );
    });
});

describe("view exists", () => {
    beforeEach(() => {
        assert.commandWorked(testDB.runCommand({create: collName, viewOn: otherName, pipeline: viewPipeline}));
        assertCollExists(true, testDB, collName);
        assertCollExists(false, testDB, bucketsName);
        assertCollExists(false, testDB, otherName);
    });
    afterEach(() => {
        assertCollExists(true, testDB, collName);
        assertCollExists(false, testDB, bucketsName);
        assertCollExists(false, testDB, otherName);
        dropMainNamespace();
    });
    it("Create non-timeseries coll", () => {
        // Creating a non-timeseries collection when a view already exists must fail.
        assert.commandFailedWithCode(testDB.createCollection(collName), ErrorCodes.NamespaceExists);
    });
    it("Create timeseries coll", () => {
        // Create timeseries collection when a view already exists on namespace.
        // Command should fail with NamespaceExists
        assert.commandFailedWithCode(
            testDB.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
            ErrorCodes.NamespaceExists,
        );
    });
    it("Create view - idempotent", () => {
        assert.commandWorked(testDB.runCommand({create: collName, viewOn: otherName, pipeline: viewPipeline}));
    });
    it("Create view - different target namespace", () => {
        assert.commandFailedWithCode(
            testDB.runCommand({create: collName, viewOn: "differentNamespace", pipeline: viewPipeline}),
            ErrorCodes.NamespaceExists,
        );
    });
    it("Create view - different pipeline", () => {
        assert.commandFailedWithCode(
            testDB.runCommand({create: collName, viewOn: otherName, pipeline: []}),
            ErrorCodes.NamespaceExists,
        );
    });
});
