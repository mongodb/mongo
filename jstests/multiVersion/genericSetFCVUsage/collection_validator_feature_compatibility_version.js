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

// The 'testCases' array should be populated with
//
//      { validator: { ... }, nonMatchingDocument: { ... } }
//
// objects that use query features new in the latest version of mongod. Note that this also
// includes new aggregation expressions able to be used with the $expr match expression. This
// test ensures that a collection validator accepts the new query feature when the feature
// compatibility version is the latest version, and rejects it when the feature compatibility
// version is the last-stable version.
const testCases = [];

let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
assert.neq(null, conn, "mongod was unable to start up");

let testDB = conn.getDB(testName);

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

// Set the feature compatibility version to the last-stable version.
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));

testCases.forEach(function(test, i) {
    // The validator is already in place, so it should still cause this insert to fail.
    const coll = testDB["coll" + i];
    assert.writeErrorWithCode(
        coll.insert(test.nonMatchingDocument),
        ErrorCodes.DocumentValidationFailure,
        `Expected document ${tojson(test.nonMatchingDocument)} to fail validation for ` +
            `collection with validator ${tojson(test.validator)}`);

    // Trying to create a new collection with a validator using new query features should fail
    // while feature compatibility version is the last-stable version.
    let res = testDB.createCollection("other", {validator: test.validator});
    assert.commandFailedWithCode(
        res,
        ErrorCodes.QueryFeatureNotAllowed,
        'Expected *not* to be able to create collection with validator ' + tojson(test.validator));
    assert(res.errmsg.match(/feature compatibility version/),
           `Expected error message from createCollection with validator ` +
               `${tojson(test.validator)} to reference 'feature compatibility version' but got: ` +
               res.errmsg);

    // Trying to update a collection with a validator using new query features should also fail.
    res = testDB.runCommand({collMod: coll.getName(), validator: test.validator});
    assert.commandFailedWithCode(
        res,
        ErrorCodes.QueryFeatureNotAllowed,
        `Expected to be able to create collection with validator ${tojson(test.validator)}`);
    assert(res.errmsg.match(/feature compatibility version/),
           `Expected error message from createCollection with validator ` +
               `${tojson(test.validator)} to reference 'feature compatibility version' but got: ` +
               res.errmsg);
});

MongoRunner.stopMongod(conn);

if (testCases.length > 0) {
    // If we try to start up the last-stable version of mongod, it will fail, because it will
    // not be able to parse the validator using new query features.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "last-stable", noCleanData: true});
    assert.eq(null,
              conn,
              `version ${MongoRunner.getBinVersionFor("last-stable")} of mongod started, even` +
                  " with a validator using new query features in place.");
}

// Starting up the latest version of mongod, however, should succeed, even though the feature
// compatibility version is still set to the last-stable version.
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

// Now, we should be able to start up the last-stable version of mongod.
conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "last-stable", noCleanData: true});
assert.neq(
    null,
    conn,
    `version ${MongoRunner.getBinVersionFor("last-stable")} of mongod failed to start, even` +
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

// Set the feature compatibility version to the last-stable version and then restart with
// internalValidateFeaturesAsMaster=false.
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({
    dbpath: dbpath,
    binVersion: "latest",
    noCleanData: true,
    setParameter: "internalValidateFeaturesAsMaster=false"
});
assert.neq(null, conn, "mongod was unable to start up");

testDB = conn.getDB(testName);

testCases.forEach(function(test, i) {
    const coll = testDB["coll3" + i];
    // Even though the feature compatibility version is the last-stable version, we should still
    // be able to add a validator using new query features, because
    // internalValidateFeaturesAsMaster is false.
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
}());
