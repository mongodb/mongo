/**
 * This test verifies that applyOps with uuid is rejected when the server is in
 * full-downgraded FCV3.4.
 */
(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    let dbpath = MongoRunner.dataPath + "apply_ops_after_downgrade";
    resetDbpath(dbpath);

    let conn = MongoRunner.runMongod({dbpath: dbpath});
    assert.neq(null, conn, "mongod was unable to start up");
    let adminDB = conn.getDB("admin");

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    checkFCV(adminDB, "3.4");
    assert.commandFailedWithCode(
        adminDB.runCommand({applyOps: [{op: 'c', ns: "test.$cmd", ui: UUID(), o: {create: "z"}}]}),
        ErrorCodes.OplogOperationUnsupported);

    MongoRunner.stopMongod(conn);
})();
