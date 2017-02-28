// Tests that manipulating the featureCompatibilityVersion document in admin.system.version changes
// the value of the featureCompatibilityVersion server parameter.

(function() {
    "use strict";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    let adminDB = conn.getDB("admin");

    // Initially the featureCompatibilityVersion is 3.4.
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.4", res.featureCompatibilityVersion);

    // Updating the featureCompatibilityVersion document changes the featureCompatibilityVersion
    // server parameter.
    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: "3.2"}}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    assert.writeOK(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                 {$set: {version: "3.4"}}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.4", res.featureCompatibilityVersion);

    // Updating the featureCompatibilityVersion document with an invalid version fails.
    assert.writeErrorWithCode(adminDB.system.version.update({_id: "featureCompatibilityVersion"},
                                                            {$set: {version: "3.6"}}),
                              ErrorCodes.BadValue);
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.4", res.featureCompatibilityVersion);

    // Deleting the featureCompatibilityVersion document changes the featureCompatibilityVersion
    // server parameter to 3.2.
    assert.writeOK(adminDB.system.version.remove({_id: "featureCompatibilityVersion"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    // Inserting a featureCompatibilityVersion document with an invalid version fails.
    assert.writeErrorWithCode(
        adminDB.system.version.insert({_id: "featureCompatibilityVersion", version: "3.6"}),
        ErrorCodes.BadValue);
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    // Inserting the featureCompatibilityVersion document changes the featureCompatibilityVersion
    // server parameter.
    assert.writeOK(
        adminDB.system.version.insert({_id: "featureCompatibilityVersion", version: "3.2"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    assert.writeOK(adminDB.system.version.remove({_id: "featureCompatibilityVersion"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    assert.writeOK(
        adminDB.system.version.insert({_id: "featureCompatibilityVersion", version: "3.4"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.4", res.featureCompatibilityVersion);

    // Dropping the admin database changes the featureCompatibilityVersion server parameter to 3.2.
    adminDB.dropDatabase();
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    MongoRunner.stopMongod(conn);
}());
