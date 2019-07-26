/**
 * Test that mongod will not allow creation of a view using new aggregation features when the
 * feature compatibility version is older than the latest version.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/feature_compatibility_version.js");

const testName = "view_definition_feature_compatibility_version_multiversion";
const dbpath = MongoRunner.dataPath + testName;

// The 'pipelinesWithNewFeatures' array should be populated with aggregation pipelines that use
// aggregation features new in the latest version of mongod. This test ensures that a view
// definition accepts the new aggregation feature when the feature compatibility version is the
// latest version, and rejects it when the feature compatibility version is the last-stable
// version.
const pipelinesWithNewFeatures = [];

let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
assert.neq(null, conn, "mongod was unable to start up");
let testDB = conn.getDB(testName);

// Explicitly set feature compatibility version to the latest version.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Test that we are able to create a new view with any of the new features.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandWorked(
        testDB.createView("firstView" + i, "coll", pipe),
        `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
            ` ${latestFCV}`));

// Test that we are able to create a new view with any of the new features.
pipelinesWithNewFeatures.forEach(function(pipe, i) {
    assert(testDB["firstView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
    assert.commandWorked(testDB.createView("firstView" + i, "coll", []));
    assert.commandWorked(
        testDB.runCommand({collMod: "firstView" + i, viewOn: "coll", pipeline: pipe}),
        `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV` +
            ` ${latestFCV}`);
});

// Create an empty view which we will attempt to update to use new query features while the
// feature compatibility version is the last-stable version.
assert.commandWorked(testDB.createView("emptyView", "coll", []));

// Set the feature compatibility version to the last-stable version.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Read against an existing view using new query features should not fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                      `Failed to query view with pipeline ${tojson(pipe)}`));

// Trying to create a new view using new query features should fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandFailedWithCode(
        testDB.createView("view_fail" + i, "coll", pipe),
        ErrorCodes.QueryFeatureNotAllowed,
        `Expected *not* to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
            ` ${lastStableFCV}`));

// Trying to update existing view to use new query features should also fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandFailedWithCode(
        testDB.runCommand({collMod: "emptyView", viewOn: "coll", pipeline: pipe}),
        ErrorCodes.QueryFeatureNotAllowed,
        `Expected *not* to be able to modify view to use pipeline ${tojson(pipe)} while in` +
            `FCV ${lastStableFCV}`));

MongoRunner.stopMongod(conn);

// Starting up the last-stable version of mongod with new query features will succeed.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "last-stable", noCleanData: true});
assert.neq(null,
           conn,
           `version ${MongoRunner.getBinVersionFor("last-stable")} of mongod was` +
               " unable to start up");
testDB = conn.getDB(testName);

// Reads will fail against views with new query features when running the last-stable version.
// Not checking the code returned on failure as it is not uniform across the various
// 'pipeline' arguments tested.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandFailed(
        testDB.runCommand({find: "firstView" + i}),
        `Expected read against view with pipeline ${tojson(pipe)} to fail on version` +
            ` ${MongoRunner.getBinVersionFor("last-stable")}`));

// Test that a read against a view that does not contain new query features succeeds.
assert.commandWorked(testDB.runCommand({find: "emptyView"}));

MongoRunner.stopMongod(conn);

// Starting up the latest version of mongod should succeed, even though the feature
// compatibility version is still set to the last-stable version.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
assert.neq(null, conn, "mongod was unable to start up");
testDB = conn.getDB(testName);

// Read against an existing view using new query features should not fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                      `Failed to query view with pipeline ${tojson(pipe)}`));

// Set the feature compatibility version back to the latest version.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

pipelinesWithNewFeatures.forEach(function(pipe, i) {
    assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                         `Failed to query view with pipeline ${tojson(pipe)}`);
    // Test that we are able to create a new view with any of the new features.
    assert.commandWorked(
        testDB.createView("secondView" + i, "coll", pipe),
        `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
            ` ${latestFCV}`);

    // Test that we are able to update an existing view to use any of the new features.
    assert(testDB["secondView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
    assert.commandWorked(testDB.createView("secondView" + i, "coll", []));
    assert.commandWorked(
        testDB.runCommand({collMod: "secondView" + i, viewOn: "coll", pipeline: pipe}),
        `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV` +
            ` ${latestFCV}`);
});

// Set the feature compatibility version to the last-stable version and then restart with
// internalValidateFeaturesAsMaster=false.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({
    dbpath: dbpath,
    binVersion: "latest",
    noCleanData: true,
    setParameter: "internalValidateFeaturesAsMaster=false"
});
assert.neq(null, conn, "mongod was unable to start up");
testDB = conn.getDB(testName);

pipelinesWithNewFeatures.forEach(function(pipe, i) {
    // Even though the feature compatibility version is the last-stable version, we should still
    // be able to create a view using new query features, because
    // internalValidateFeaturesAsMaster is false.
    assert.commandWorked(
        testDB.createView("thirdView" + i, "coll", pipe),
        `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
            ` ${lastStableFCV} with internalValidateFeaturesAsMaster=false`);

    // We should also be able to modify a view to use new query features.
    assert(testDB["thirdView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
    assert.commandWorked(testDB.createView("thirdView" + i, "coll", []));
    assert.commandWorked(
        testDB.runCommand({collMod: "thirdView" + i, viewOn: "coll", pipeline: pipe}),
        `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV` +
            ` ${lastStableFCV} with internalValidateFeaturesAsMaster=false`);
});

MongoRunner.stopMongod(conn);

// Starting up the last-stable version of mongod with new query features should succeed.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "last-stable", noCleanData: true});
assert.neq(null,
           conn,
           `version ${MongoRunner.getBinVersionFor("last-stable")} of mongod was` +
               " unable to start up");
testDB = conn.getDB(testName);

// Existing views with new query features can be dropped.
pipelinesWithNewFeatures.forEach((pipe, i) =>
                                     assert(testDB["firstView" + i].drop(),
                                            `Drop of view with pipeline ${tojson(pipe)} failed`));
assert(testDB.system.views.drop(), "Drop of system.views collection failed");

MongoRunner.stopMongod(conn);
}());
