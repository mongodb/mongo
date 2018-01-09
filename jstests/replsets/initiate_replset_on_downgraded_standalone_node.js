/**
 * Restarting a downgraded standalone node as a replset node
 * shouldn't assign UUID for collections.
 *
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/uuid_util.js");
    load("jstests/replsets/rslib.js");

    let dbpath = MongoRunner.dataPath + "initiate_replset_on_downgraded_standalone_node";
    let conn = MongoRunner.runMongod({dbpath: dbpath});
    assert.neq(null, conn, "mongod was unable to start up");
    let adminDB = conn.getDB("admin");

    // Downgrade the server.
    let versionWithoutUUIDSupport = "3.4";
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: versionWithoutUUIDSupport}));
    checkFCV(adminDB, versionWithoutUUIDSupport);

    MongoRunner.stopMongod(conn);

    // Restart the mongod but with --replSet this time.
    conn = MongoRunner.runMongod({dbpath: dbpath, "replSet": "rs0", noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");
    adminDB = conn.getDB("admin");
    let replSetConfig = {_id: "rs0", members: [{_id: 0, host: conn.host}]};
    let cmd = {replSetInitiate: replSetConfig};
    assert.commandWorked(adminDB.runCommand(cmd), tojson(cmd));

    // Wait until the node becomes the primary.
    waitForState(conn, ReplSetTest.State.PRIMARY);

    // Check an arbitrary collection's uuid field.
    assert.eq(undefined, getUUIDFromListCollections(conn.getDB("local"), "startup_log"));
}());
