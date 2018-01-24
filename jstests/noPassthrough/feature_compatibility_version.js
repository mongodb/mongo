// Tests that manipulating the featureCompatibilityVersion document in admin.system.version changes
// the value of the featureCompatibilityVersion server parameter.

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    // Skip collection validation since this test leaves collections in an invalid state, where
    // FCV=3.4 but UUIDs exist. (Modifying the FCV document directly does not perform
    // upgrade/downgrade with UUID addition/removal, but merely modifies the in-memory variable.)
    // TODO: remove this in SERVER-32597, when FCV 3.4 is removed and UUIDs always exist.
    TestData.skipCollectionAndIndexValidation = true;

    let latestFCV = "4.0";
    let lastStableFCV = "3.6";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    let adminDB = conn.getDB("admin");

    // TODO: remove this when the FCV default is bumped (SERVER-32597).
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Initially the featureCompatibilityVersion is latestFCV.
    checkFCV(adminDB, latestFCV);

    // Updating the featureCompatibilityVersion document changes the featureCompatibilityVersion
    // server parameter.
    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: lastStableFCV}}));
    checkFCV(adminDB, lastStableFCV);

    assert.writeOK(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                      {$set: {version: lastStableFCV, targetVersion: latestFCV}}));
    checkFCV(adminDB, lastStableFCV, latestFCV);

    assert.writeOK(adminDB.system.version.update(
        {_id: "featureCompatibilityVersion"},
        {$set: {version: lastStableFCV, targetVersion: lastStableFCV}}));
    checkFCV(adminDB, lastStableFCV, lastStableFCV);

    assert.writeOK(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                      {$set: {version: latestFCV}, $unset: {targetVersion: true}}));
    checkFCV(adminDB, latestFCV);

    // Updating the featureCompatibilityVersion document with an invalid version fails.
    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {version: "3.2"}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, latestFCV);

    // Updating the featureCompatibilityVersion document with an invalid targetVersion fails.
    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {targetVersion: lastStableFCV}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, latestFCV);

    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {targetVersion: latestFCV}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, latestFCV);

    // TODO: remove these FCV 3.4 / 4.0 tests when FCV 3.4 is removed (SERVER-32597).
    // ----------------------------------------------------------------------------------

    // Cannot be in FCV 4.0 and target 3.4.
    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {targetVersion: "3.4"}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, latestFCV);

    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: lastStableFCV}}));
    checkFCV(adminDB, lastStableFCV);
    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: "3.4"}}));
    checkFCV(adminDB, "3.4");

    // Cannot be in FCV 3.4 and target 4.0.
    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {targetVersion: latestFCV}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, "3.4");
    // ------------------ end FCV 3.4/4.0 testing ---------------------------------------

    MongoRunner.stopMongod(conn);
}());
