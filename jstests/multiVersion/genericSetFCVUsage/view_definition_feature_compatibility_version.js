/**
 * Test that mongod will not allow creation of a view using new aggregation features when the
 * feature compatibility version is older than the latest version.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

const testName = "view_definition_feature_compatibility_version_multiversion";
const dbpath = MongoRunner.dataPath + testName;

// An array of feature flags that must be enabled to run feature flag tests.
const featureFlagsToEnable = [];

// These arrays should be populated with aggregation pipelines that use
// aggregation features in new versions of mongod. This test ensures that a view
// definition accepts the new aggregation feature when the feature compatibility version is the
// latest version, and rejects it when the feature compatibility version is the last
// version.
const testCasesLastContinuous = [];
const testCasesLastContinuousWithFeatureFlags = [];

// Anything that's incompatible with the last continuous release is incompatible with the last
// stable release.
const testCasesLastStable = testCasesLastContinuous.concat([
    [
        // TODO SERVER-53028: Remove these cases when 5.0 becomes lastLTS.
        {
            $project: {
                x: {
                    $dateDiff: {
                        startDate: new Date("2020-02-02T02:02:02"),
                        endDate: new Date("2020-02-02T03:02:02"),
                        unit: "hour"
                    }
                }
            }
        }
    ],
    [{
        $project:
            {y: {$dateAdd: {startDate: new Date("2020-02-02T02:02:02"), unit: "week", amount: 1}}}
    }],
    [{
        $project: {
            z: {
                $dateSubtract: {startDate: new Date("2020-02-02T02:02:02"), unit: "hour", amount: 3}
            }
        }
    }],
    [{$project: {x: {$dateTrunc: {date: new Date("2020-02-02T02:02:02"), unit: "month"}}}}],
    [{$group: {_id: null, count: {$count: {}}}}],
    [{$bucket: {groupBy: "$a", boundaries: [0, 1], output: {count: {$count: {}}}}}],
]);

const testCasesLastStableWithFeatureFlags = testCasesLastContinuousWithFeatureFlags.concat([
    [{$setWindowFields: {sortBy: {_id: 1}, output: {sum: {$sum: {input: "$val"}}}}}],
]);

// Tests Feature Compatibility Version behavior of view creation while using aggregation pipelines
// 'testCases' and using a previous stable version 'lastVersion' of mongod.
// 'lastVersion' can have values "last-lts" and "last-continuous".
function testViewDefinitionFCVBehavior(lastVersion, testCases, featureFlags = []) {
    if (testCases.length === 0) {
        jsTestLog("Skipping setup for tests against " + lastVersion + " since there are none");
        return;
    }

    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
    assert.neq(null, conn, "mongod was unable to start up");
    let testDB = conn.getDB(testName);
    for (let i = 0; i < featureFlags.length; i++) {
        const command = {"getParameter": 1};
        command[featureFlags[i]] = 1;
        const featureEnabled =
            assert.commandWorked(testDB.adminCommand(command))[featureFlags[i]].value;
        if (!featureEnabled) {
            jsTestLog("Skipping test because the " + featureFlags[i] + " feature flag is disabled");
            MongoRunner.stopMongod(conn);
            return;
        }
    }

    // Explicitly set feature compatibility version to the latest version.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Test that we are able to create a new view with any of the new features.
    testCases.forEach(
        (pipe, i) => assert.commandWorked(
            testDB.createView("firstView" + i, "coll", pipe),
            `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
                ` ${latestFCV}`));

    // Test that we are able to update an existing view with any of the new features.
    testCases.forEach(function(pipe, i) {
        assert(testDB["firstView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
        assert.commandWorked(testDB.createView("firstView" + i, "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "firstView" + i, viewOn: "coll", pipeline: pipe}),
            `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV` +
                ` ${latestFCV}`);
    });

    // Create an empty view which we will attempt to update to use new query features while the
    // feature compatibility version is the last version.
    assert.commandWorked(testDB.createView("emptyView", "coll", []));

    // Set the feature compatibility version to the last version.
    assert.commandWorked(
        testDB.adminCommand({setFeatureCompatibilityVersion: binVersionToFCV(lastVersion)}));

    // Read against an existing view using new query features should not fail.
    testCases.forEach(
        (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                          `Failed to query view with pipeline ${tojson(pipe)}`));

    // Trying to create a new view in the same database as existing invalid view should fail,
    // even if the new view doesn't use any new query features.
    assert.commandFailedWithCode(
        testDB.createView("newViewOldFeatures", "coll", [{$project: {_id: 1}}]),
        ErrorCodes.QueryFeatureNotAllowed,
        `Expected *not* to be able to create view on database ${testDB} while in FCV ${
            binVersionToFCV(lastVersion)}`);

    // Trying to create a new view succeeds if it's on a separate database.
    const testDB2 = conn.getDB(testName + '2');
    assert.commandWorked(testDB2.dropDatabase());
    assert.commandWorked(testDB2.createView("newViewOldFeatures", "coll", [{$project: {_id: 1}}]));

    // Trying to create a new view using new query features should fail.
    // (We use a separate DB to ensure this can only fail because of the view we're trying to
    // create, as opposed to an existing view.)
    testCases.forEach(
        (pipe, i) => assert.commandFailedWithCode(
            testDB2.createView("view_fail" + i, "coll", pipe),
            ErrorCodes.QueryFeatureNotAllowed,
            `Expected *not* to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
                ` ${binVersionToFCV(lastVersion)}`));

    // Trying to update existing view to use new query features should also fail.
    testCases.forEach(
        (pipe, i) => assert.commandFailedWithCode(
            testDB.runCommand({collMod: "emptyView", viewOn: "coll", pipeline: pipe}),
            ErrorCodes.QueryFeatureNotAllowed,
            `Expected *not* to be able to modify view to use pipeline ${tojson(pipe)} while in` +
                `FCV ${binVersionToFCV(lastVersion)}`));

    MongoRunner.stopMongod(conn);

    // Starting up the last version of mongod with new query features will succeed.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastVersion, noCleanData: true});
    assert.neq(null,
               conn,
               `version ${MongoRunner.getBinVersionFor(lastVersion)} of mongod was` +
                   " unable to start up");
    testDB = conn.getDB(testName);

    // Reads will fail against views with new query features when running the last version.
    // Not checking the code returned on failure as it is not uniform across the various
    // 'pipeline' arguments tested.
    testCases.forEach(
        (pipe, i) => assert.commandFailed(
            testDB.runCommand({find: "firstView" + i}),
            `Expected read against view with pipeline ${tojson(pipe)} to fail on version` +
                ` ${MongoRunner.getBinVersionFor(lastVersion)}`));

    // Test that a read against a view that does not contain new query features succeeds.
    assert.commandWorked(testDB.runCommand({find: "emptyView"}));

    MongoRunner.stopMongod(conn);

    // Starting up the latest version of mongod should succeed, even though the feature
    // compatibility version is still set to the last version.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");
    testDB = conn.getDB(testName);

    // Read against an existing view using new query features should not fail.
    testCases.forEach(
        (pipe, i) => assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                                          `Failed to query view with pipeline ${tojson(pipe)}`));

    // Set the feature compatibility version back to the latest version.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    testCases.forEach(function(pipe, i) {
        assert.commandWorked(testDB.runCommand({find: "firstView" + i}),
                             `Failed to query view with pipeline ${tojson(pipe)}`);
        // Test that we are able to create a new view with any of the new features.
        assert.commandWorked(
            testDB.createView("secondView" + i, "coll", pipe),
            `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
                ` ${latestFCV}`);

        // Test that we are able to update an existing view to use any of the new features.
        assert(testDB["secondView" + i].drop(),
               `Drop of view with pipeline ${tojson(pipe)} failed`);
        assert.commandWorked(testDB.createView("secondView" + i, "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "secondView" + i, viewOn: "coll", pipeline: pipe}),
            `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV` +
                ` ${latestFCV}`);
    });

    // Set the feature compatibility version to the last version and then restart with
    // internalValidateFeaturesAsPrimary=false.
    assert.commandWorked(
        testDB.adminCommand({setFeatureCompatibilityVersion: binVersionToFCV(lastVersion)}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        binVersion: "latest",
        noCleanData: true,
        setParameter: "internalValidateFeaturesAsPrimary=false"
    });
    assert.neq(null, conn, "mongod was unable to start up");
    testDB = conn.getDB(testName);

    testCases.forEach(function(pipe, i) {
        // Even though the feature compatibility version is the last version, we should still be
        // able to create a view using new query features, because internalValidateFeaturesAsPrimary
        // is false.
        assert.commandWorked(
            testDB.createView("thirdView" + i, "coll", pipe),
            `Expected to be able to create view with pipeline ${tojson(pipe)} while in FCV` +
                ` ${binVersionToFCV(lastVersion)} with internalValidateFeaturesAsPrimary=false`);

        // We should also be able to modify a view to use new query features.
        assert(testDB["thirdView" + i].drop(), `Drop of view with pipeline ${tojson(pipe)} failed`);
        assert.commandWorked(testDB.createView("thirdView" + i, "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "thirdView" + i, viewOn: "coll", pipeline: pipe}),
            `Expected to be able to modify view to use pipeline ${tojson(pipe)} while in FCV` +
                ` ${binVersionToFCV(lastVersion)} with internalValidateFeaturesAsPrimary=false`);
    });

    MongoRunner.stopMongod(conn);

    // Starting up the last version of mongod with new query features should succeed.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastVersion, noCleanData: true});
    assert.neq(null,
               conn,
               `version ${MongoRunner.getBinVersionFor(lastVersion)} of mongod was` +
                   " unable to start up");
    testDB = conn.getDB(testName);

    // Existing views with new query features can be dropped.
    testCases.forEach((pipe, i) => assert(testDB["firstView" + i].drop(),
                                          `Drop of view with pipeline ${tojson(pipe)} failed`));
    assert(testDB.system.views.drop(), "Drop of system.views collection failed");

    MongoRunner.stopMongod(conn);
}

testViewDefinitionFCVBehavior("last-lts", testCasesLastStable);
testViewDefinitionFCVBehavior(
    "last-lts", testCasesLastStableWithFeatureFlags, featureFlagsToEnable);
testViewDefinitionFCVBehavior("last-continuous", testCasesLastContinuous);
testViewDefinitionFCVBehavior(
    "last-continuous", testCasesLastContinuousWithFeatureFlags, featureFlagsToEnable);
}());
