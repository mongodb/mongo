/**
 * Test that mongod will not allow creation of collection validators using new query features when
 * the feature compatibility version is older than the latest version.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

const testName = "collection_validator_feature_compatibility_version";
const dbpath = MongoRunner.dataPath + testName;

// An array of feature flags that must be enabled to run feature flag tests.
const featureFlagsToEnable = [];

// These arrays should be populated with
//
//      { validator: { ... }, nonMatchingDocument: { ... }, lastStableErrCode }
//
// objects that use query features in new versions of mongod. Note that this also
// includes new aggregation expressions able to be used with the $expr match expression. This
// test ensures that a collection validator accepts the new query feature when the feature
// compatibility version is the latest version, and rejects it when the feature compatibility
// version is the last version.
// The 'lastStableErrCode' field indicates what error the last version would throw when
// parsing the validator.
const testCasesLastContinuous = [
    //
    // Populate with any new expressions.
    //
];
const testCasesLastContinuousWithFeatureFlags = [];

const testCasesLastStable = testCasesLastContinuous.concat([
    // These expressions were introduced in 4.9.
    // TODO SERVER-53028: Remove these cases when 5.0 becomes lastLTS.
    {
        validator: {
            $expr: {
                $eq: [
                    {
                        $dateDiff: {
                            startDate: new Date("2020-02-02T02:02:02"),
                            endDate: new Date("2020-02-02T03:02:02"),
                            unit: "hour"
                        }
                    },
                    0
                ]
            }
        },
        nonMatchingDocument: {a: 1},
        lastStableErrCode: 168
    },
    {
        validator: {
            $expr: {
                $eq: [
                    {
                        $dateAdd:
                            {startDate: new Date("2020-10-10T10:00:00"), unit: "hour", amount: 1}
                    },
                    new Date("2020-10-10T10:00:00")
                ]
            }
        },
        nonMatchingDocument: {a: 1},
        lastStableErrCode: 168
    },
    {
        validator: {
            $expr: {
                $eq: [
                    {
                        $dateSubtract:
                            {startDate: new Date("2020-10-10T10:00:00"), unit: "hour", amount: 1}
                    },
                    new Date("2020-10-10T10:00:00")
                ]
            }
        },
        nonMatchingDocument: {a: 1},
        lastStableErrCode: 168
    },
    {
        validator: {
            $expr: {
                $eq: [
                    {$dateTrunc: {date: new Date("2020-02-02T02:02:02"), unit: "hour"}},
                    new Date("2020-02-02T02:02:02")
                ]
            }
        },
        nonMatchingDocument: {a: 1},
        lastStableErrCode: 168
    },
]);
const testCasesLastStableWithFeatureFlags = testCasesLastContinuousWithFeatureFlags.concat([]);

// Tests Feature Compatibility Version behavior of the validator of a collection by executing test
// cases 'testCases' and using a previous stable version 'lastVersion' of mongod. 'lastVersion' can
// have values "last-lts" and "last-continuous".
function testCollectionValidatorFCVBehavior(lastVersion, testCases, featureFlags = []) {
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

    let adminDB = conn.getDB("admin");

    // Explicitly set the feature compatibility version to the latest version.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    testCases.forEach(function(test, i) {
        // Create a collection with a validator using new query features.
        const coll = testDB["coll" + i];
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {validator: test.validator}),
            `Expected to be able to create collection with validator ${tojson(test.validator)}`);

        // The validator should cause this insert to fail.
        assert.writeErrorWithCode(
            coll.insert(test.nonMatchingDocument),
            ErrorCodes.DocumentValidationFailure,
            `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                `collection with validator ${tojson(test.validator)}`);

        // Set a validator using new query features on an existing collection.
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName()));
        assert.commandWorked(
            testDB.runCommand({collMod: coll.getName(), validator: test.validator}),
            `Expected to be able to modify collection validator to be ${tojson(test.validator)}`);

        // Another failing update.
        assert.writeErrorWithCode(
            coll.insert(test.nonMatchingDocument),
            ErrorCodes.DocumentValidationFailure,
            `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                `collection with validator ${tojson(test.validator)}`);
    });

    // Set the feature compatibility version to the last version.
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: binVersionToFCV(lastVersion)}));

    testCases.forEach(
        function(test, i) {
            // The validator is already in place, so it should still cause this insert to fail.
            const coll = testDB["coll" + i];
            assert.writeErrorWithCode(
                coll.insert(test.nonMatchingDocument),
                ErrorCodes.DocumentValidationFailure,
                `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                    `collection with validator ${tojson(test.validator)}`);

            // Trying to create a new collection with a validator using new query features should
            // fail while feature compatibility version is the last version.
            let res = testDB.createCollection("other", {validator: test.validator});
            assert.commandFailedWithCode(
                res,
                ErrorCodes.QueryFeatureNotAllowed,
                'Expected *not* to be able to create collection with validator ' +
                    tojson(test.validator));
            assert(res.errmsg.match(/feature compatibility version/),
           `Expected error message from createCollection with validator ` +
               `${tojson(test.validator)} to reference 'feature compatibility version' but got: ` +
               res.errmsg);

            // Trying to update a collection with a validator using new query features should also
            // fail.
            res = testDB.runCommand({collMod: coll.getName(), validator: test.validator});
            assert.commandFailedWithCode(res,
                                         ErrorCodes.QueryFeatureNotAllowed,
                                         `Expected to be able to create collection with validator ${
                                             tojson(test.validator)}`);
            assert(res.errmsg.match(/feature compatibility version/),
           `Expected error message from createCollection with validator ` +
               `${tojson(test.validator)} to reference 'feature compatibility version' but got: ` +
               res.errmsg);
        });

    MongoRunner.stopMongod(conn);

    if (testCases.length > 0) {
        // Versions of mongod 4.2 and later are able to start up with a collection validator that's
        // considered invalid. However, any writes to the collection will fail.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastVersion, noCleanData: true});
        assert.neq(
            null, conn, lastVersion + " mongod was unable to start up with invalid validator");
        const testDB = conn.getDB(testName);

        // Check that writes fail to all collections with validators using new query features.
        testCases.forEach(function(test, i) {
            const coll = testDB["coll" + i];
            assert.commandFailedWithCode(coll.insert({foo: 1}), test.lastStableErrCode);
        });

        MongoRunner.stopMongod(conn);
    }

    // Starting up the latest version of mongod, however, should succeed, even though the feature
    // compatibility version is still set to the last version.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");

    adminDB = conn.getDB("admin");
    testDB = conn.getDB(testName);

    // And the validator should still work.
    testCases.forEach(function(test, i) {
        const coll = testDB["coll" + i];
        assert.writeErrorWithCode(
            coll.insert(test.nonMatchingDocument),
            ErrorCodes.DocumentValidationFailure,
            `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
                `collection with validator ${tojson(test.validator)}`);

        // Remove the validator.
        assert.commandWorked(testDB.runCommand({collMod: coll.getName(), validator: {}}));
    });

    MongoRunner.stopMongod(conn);

    // Now, we should be able to start up the last version of mongod.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastVersion, noCleanData: true});
    assert.neq(
        null,
        conn,
        `version ${MongoRunner.getBinVersionFor(lastVersion)} of mongod failed to start, even` +
            " after we removed the validator using new query features");

    MongoRunner.stopMongod(conn);

    // The rest of the test uses the latest version of mongod.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");

    adminDB = conn.getDB("admin");
    testDB = conn.getDB(testName);

    // Set the feature compatibility version back to the latest version.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    testCases.forEach(function(test, i) {
        const coll = testDB["coll2" + i];

        // Now we should be able to create a collection with a validator using new query features
        // again.
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {validator: test.validator}),
            `Expected to be able to create collection with validator ${tojson(test.validator)}`);

        // And we should be able to modify a collection to have a validator using new query
        // features.
        assert.commandWorked(
            testDB.runCommand({collMod: coll.getName(), validator: test.validator}),
            `Expected to be able to modify collection validator to be ${tojson(test.validator)}`);
    });

    // Set the feature compatibility version to the last version and then restart with
    // internalValidateFeaturesAsPrimary=false.
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: binVersionToFCV(lastVersion)}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        binVersion: "latest",
        noCleanData: true,
        setParameter: "internalValidateFeaturesAsPrimary=false"
    });
    assert.neq(null, conn, "mongod was unable to start up");

    testDB = conn.getDB(testName);

    testCases.forEach(function(test, i) {
        const coll = testDB["coll3" + i];
        // Even though the feature compatibility version is the last version, we should still
        // be able to add a validator using new query features, because
        // internalValidateFeaturesAsPrimary is false.
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {validator: test.validator}),
            `Expected to be able to create collection with validator ${tojson(test.validator)}`);

        // We should also be able to modify a collection to have a validator using new query
        // features.
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName()));
        assert.commandWorked(
            testDB.runCommand({collMod: coll.getName(), validator: test.validator}),
            `Expected to be able to modify collection validator to be ${tojson(test.validator)}`);
    });

    MongoRunner.stopMongod(conn);
}

testCollectionValidatorFCVBehavior("last-lts", testCasesLastStable);
testCollectionValidatorFCVBehavior(
    "last-lts", testCasesLastStableWithFeatureFlags, featureFlagsToEnable);
testCollectionValidatorFCVBehavior("last-continuous", testCasesLastContinuous);
testCollectionValidatorFCVBehavior(
    "last-continuous", testCasesLastContinuousWithFeatureFlags, featureFlagsToEnable);
}());
