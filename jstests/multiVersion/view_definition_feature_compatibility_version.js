/**
 * Test that mongod will not allow creation of a view using 4.2 aggregation features when the
 * feature compatibility version is older than 4.2.
 *
 * TODO SERVER-41273: Remove FCV 4.0 validation during the 4.3 development cycle.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

const testName = "view_definition_feature_compatibility_version_multiversion";
const dbpath = MongoRunner.dataPath + testName;

// In order to avoid restarting the server for each test case, we declare all the test cases up
// front, and test them all at once.
const pipelinesWithNewFeatures = [
        [{$addFields: {x: {$round: 4.57}}}],
        [{$addFields: {x: {$trunc: [4.57, 1]}}}],
        [{$addFields: {x: {$regexFind: {input: "string", regex: /st/}}}}],
        [{$addFields: {x: {$regexFindAll: {input: "string", regex: /st/}}}}],
        [{$addFields: {x: {$regexMatch: {input: "string", regex: /st/}}}}],
        [{$facet: {pipe1: [{$addFields: {x: {$round: 4.57}}}]}}],
        [{
           $facet: {
               pipe1: [{$addFields: {x: {$round: 4.57}}}],
               pipe2: [{$addFields: {newThing: {$regexMatch: {input: "string", regex: /st/}}}}]
           }
        }],
        [{$lookup: {from: 'x', pipeline: [{$addFields: {x: {$round: 4.57}}}], as: 'results'}}],
        [{
           $graphLookup: {
               from: 'x',
               startWith: ["$_id"],
               connectFromField: "target_id",
               connectToField: "_id",
               restrictSearchWithMatch: {$expr: {$eq: [4, {$round: "$x"}]}},
               as: 'results'
           }
        }],
        [{
           $lookup: {
               from: 'x',
               pipeline: [{$facet: {pipe1: [{$addFields: {x: {$round: 4.57}}}]}}],
               as: 'results'
           }
        }],
    ];

let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
assert.neq(null, conn, "mongod was unable to start up");
let testDB = conn.getDB(testName);

// Explicitly set feature compatibility version 4.2.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

// Test that we are able to create a new view with any of the new features.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandWorked(
        testDB.createView("firstView" + i, "coll", pipe),
        `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV 4.2`));

// Test that we are able to create a new view with any of the new features.
pipelinesWithNewFeatures.forEach(function(pipe, i) {
    assert(testDB["firstView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
    assert.commandWorked(testDB.createView("firstView" + i, "coll", []));
    assert.commandWorked(
        testDB.runCommand({collMod: "firstView" + i, viewOn: "coll", pipeline: pipe}),
        `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV 4.2`);
});

// Create an empty view which we will attempt to update to use 4.0 query features under
// feature compatibility mode 4.0.
assert.commandWorked(testDB.createView("emptyView", "coll", []));

// Set the feature compatibility version to 4.0.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

// Read against an existing view using 4.2 query features should not fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                      `Failed to query view with pipeline ${tojson(pipe)}`));

// Trying to create a new view using 4.2 query features should fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandFailedWithCode(
        testDB.createView("view_fail" + i, "coll", pipe),
        ErrorCodes.QueryFeatureNotAllowed,
        `Expected *not* to be able to create view with pipeline ${tojson(pipe)} while in FCV 4.0`));

// Trying to update existing view to use 4.2 query features should also fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandFailedWithCode(
        testDB.runCommand({collMod: "emptyView", viewOn: "coll", pipeline: pipe}),
        ErrorCodes.QueryFeatureNotAllowed,
        `Expected *not* to be able to modify view to use pipeline ${tojson(pipe)} while in FCV
    4.0`));

MongoRunner.stopMongod(conn);

// Starting up a 4.0 mongod with 4.2 query features will succeed.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "4.0", noCleanData: true});
assert.neq(null, conn, "mongod 4.0 was unable to start up");
testDB = conn.getDB(testName);

// Reads will fail against views with 4.2 query features when running a 4.0 binary.
// Not checking the code returned on failure as it is not uniform across the various
// 'pipeline' arguments tested.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandFailed(
        testDB.runCommand({find: "firstView" + i}),
        `Expected read against view with pipeline ${tojson(pipe)} to fail on 4.0 binary`));

// Test that a read against a view that does not contain 4.2 query features succeeds.
assert.commandWorked(testDB.runCommand({find: "emptyView"}));

MongoRunner.stopMongod(conn);

// Starting up a 4.2 mongod should succeed, even though the feature compatibility version is
// still set to 4.0.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
assert.neq(null, conn, "mongod was unable to start up");
testDB = conn.getDB(testName);

// Read against an existing view using 4.2 query features should not fail.
pipelinesWithNewFeatures.forEach(
    (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                      `Failed to query view with pipeline ${tojson(pipe)}`));

// Set the feature compatibility version back to 4.2.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

pipelinesWithNewFeatures.forEach(function(pipe, i) {
    assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                         `Failed to query view with pipeline ${tojson(pipe)}`);
    // Test that we are able to create a new view with any of the new features.
    assert.commandWorked(
        testDB.createView("secondView" + i, "coll", pipe),
        `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV 4.2`);

    // Test that we are able to update an existing view to use any of the new features.
    assert(testDB["secondView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
    assert.commandWorked(testDB.createView("secondView" + i, "coll", []));
    assert.commandWorked(
        testDB.runCommand({collMod: "secondView" + i, viewOn: "coll", pipeline: pipe}),
        `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV 4.2`);
});

// Set the feature compatibility version to 4.0 and then restart with
// internalValidateFeaturesAsMaster=false.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
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
    // Even though the feature compatibility version is 4.0, we should still be able to create a
    // view using 4.2 query features, because internalValidateFeaturesAsMaster is false.
    assert.commandWorked(
        testDB.createView("thirdView" + i, "coll", pipe),
        `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV 4.0 ` +
            `with internalValidateFeaturesAsMaster=false`);

    // We should also be able to modify a view to use 4.2 query features.
    assert(testDB["thirdView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
    assert.commandWorked(testDB.createView("thirdView" + i, "coll", []));
    assert.commandWorked(
        testDB.runCommand({collMod: "thirdView" + i, viewOn: "coll", pipeline: pipe}),
        `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV 4.0 ` +
            `with internalValidateFeaturesAsMaster=false`);
});

MongoRunner.stopMongod(conn);

// Starting up a 4.0 mongod with 4.2 query features.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "4.0", noCleanData: true});
assert.neq(null, conn, "mongod 4.0 was unable to start up");
testDB = conn.getDB(testName);

// Existing views with 4.2 query features can be dropped.
pipelinesWithNewFeatures.forEach((pipe, i) =>
                                     assert(testDB["firstView" + i].drop(),
                                            `Drop of view with pipeline ${tojson(pipe)} failed`));
assert(testDB.system.views.drop(), "Drop of system.views collection failed");

MongoRunner.stopMongod(conn);
}());
