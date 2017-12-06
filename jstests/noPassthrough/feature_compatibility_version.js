// Tests that manipulating the featureCompatibilityVersion document in admin.system.version changes
// the value of the featureCompatibilityVersion server parameter.

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    /**
     * Checks that the featureCompatibilityVersion document is missing.
     */
    let checkFCVDocumentMissing = function(adminDB) {
        assert.eq(null, adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}));
    };

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    let adminDB = conn.getDB("admin");

    // Initially the featureCompatibilityVersion is 3.6.
    checkFCV(adminDB, "3.6");

    // Updating the featureCompatibilityVersion document changes the featureCompatibilityVersion
    // server parameter.
    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: "3.4"}}));
    checkFCV(adminDB, "3.4");

    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: "3.4", targetVersion: "3.6"}}));
    checkFCV(adminDB, "3.4", "3.6");

    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: "3.4", targetVersion: "3.4"}}));
    checkFCV(adminDB, "3.4", "3.4");

    assert.writeOK(
        adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                      {$set: {version: "3.6"}, $unset: {targetVersion: true}}));
    checkFCV(adminDB, "3.6");

    // Updating the featureCompatibilityVersion document with an invalid version fails.
    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {version: "3.2"}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, "3.6");

    // Updating the featureCompatibilityVersion document with an invalid targetVersion fails.
    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {targetVersion: "3.4"}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, "3.6");

    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {targetVersion: "3.6"}}),
                              ErrorCodes.BadValue);
    checkFCV(adminDB, "3.6");

    // Do hack to remove FCV document.
    removeFCVDocument(adminDB);
    checkFCVDocumentMissing(adminDB);

    // Inserting a featureCompatibilityVersion document with an invalid version fails.
    assert.writeErrorWithCode(
        adminDB.system.version.insert({_id: "featureCompatibilityVersion", version: "3.2"}),
        ErrorCodes.BadValue);
    checkFCVDocumentMissing(adminDB);

    assert.writeErrorWithCode(
        adminDB.system.version.insert({_id: "featureCompatibilityVersion", version: "3.8"}),
        ErrorCodes.BadValue);
    checkFCVDocumentMissing(adminDB);

    // Inserting a featureCompatibilityVersion document with an invalid targetVersion fails.
    assert.writeErrorWithCode(
        adminDB.system.version.insert(
            {_id: "featureCompatibilityVersion", version: "3.6", targetVersion: "3.4"}),
        ErrorCodes.BadValue);
    checkFCVDocumentMissing(adminDB);

    assert.writeErrorWithCode(
        adminDB.system.version.insert(
            {_id: "featureCompatibilityVersion", version: "3.6", targetVersion: "3.6"}),
        ErrorCodes.BadValue);
    checkFCVDocumentMissing(adminDB);

    assert.writeOK(
        adminDB.system.version.insert({_id: "featureCompatibilityVersion", version: "3.6"}));
    checkFCV(adminDB, "3.6");

    MongoRunner.stopMongod(conn);
}());
