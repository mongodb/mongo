/**
 * Test that a replica set can validate a cluster time signed by a different replica set if it has
 * the key document for that cluster time in its admin.system.external_validation_keys collection.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
"use strict";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

const kDbName = "testDb";
const kCollName = "testColl";

const kSystemUser = {
    name: "system",
    pwd: "pwd",
};
const kAdminUser = {
    name: "admin",
    pwd: "pwd",
};
const kTestUser = {
    name: "testUser",
    pwd: "pwd",
};

function createUsers(primary) {
    const adminDB = primary.getDB("admin");
    assert.commandWorked(
        adminDB.runCommand({createUser: kAdminUser.name, pwd: kAdminUser.pwd, roles: ["root"]}));
    assert.eq(1, adminDB.auth(kAdminUser.name, kAdminUser.pwd));

    assert.commandWorked(adminDB.runCommand(
        {createUser: kSystemUser.name, pwd: kSystemUser.pwd, roles: ["__system"]}));
    const testDB = primary.getDB(kDbName);
    assert.commandWorked(
        testDB.runCommand({createUser: kTestUser.name, pwd: kTestUser.pwd, roles: ["readWrite"]}));

    adminDB.logout();
}

const rst1 = new ReplSetTest({
    nodes: [
        {setParameter: {"failpoint.alwaysValidateClientsClusterTime": tojson({mode: "alwaysOn"})}}
    ],
    name: "rst1",
    keyFile: "jstests/libs/key1"
});
const rst2 = new ReplSetTest({nodes: 1, name: "rst2", keyFile: "jstests/libs/key2"});

rst1.startSet();
rst1.initiate();

rst2.startSet();
rst2.initiate();

const rst1Primary = rst1.getPrimary();
const rst2Primary = rst2.getPrimary();

const rst1AdminDB = rst1Primary.getDB("admin");
const rst2AdminDB = rst2Primary.getDB("admin");

const rst1TestDB = rst1Primary.getDB(kDbName);
const rst2TestDB = rst2Primary.getDB(kDbName);

createUsers(rst1Primary);
createUsers(rst2Primary);

jsTest.log(
    "Run commands on rst1 and rst2 such that rst2's clusterTime is greater than rst1's clusterTime");
assert.eq(1, rst1TestDB.auth(kTestUser.name, kTestUser.pwd));
assert.eq(1, rst2TestDB.auth(kTestUser.name, kTestUser.pwd));

const rst1ClusterTime = assert.commandWorked(rst1TestDB.runCommand({find: kCollName})).$clusterTime;
const rst2ClusterTime =
    assert.commandWorked(rst2TestDB.runCommand({insert: kCollName, documents: [{_id: 0}]}))
        .$clusterTime;
jsTest.log("rst1's clusterTime " + tojson(rst1ClusterTime));
jsTest.log("rst2's clusterTime " + tojson(rst2ClusterTime));

jsTest.log("Verify that rst1 fails to validate the clusterTime from rst2 at first");
// A key's keyId is generated as the node's current clusterTime's Timestamp so it is possible
// for the keyId for rst2ClusterTime to match the key on rst1, and in that case the command
// would fail with TimeProofMismatch instead of KeyNotFound.
assert.commandFailedWithCode(
    rst1TestDB.runCommand({find: kCollName, $clusterTime: rst2ClusterTime}),
    [ErrorCodes.TimeProofMismatch, ErrorCodes.KeyNotFound]);

rst1TestDB.logout();
rst2TestDB.logout();

// Copy the admin.system.keys doc for rst2's clusterTime into the
// admin.system.external_validation_keys collection on rst1. Authenticate as the __system user to
// get access to the system collections.
assert.eq(1, rst1AdminDB.auth(kSystemUser.name, kSystemUser.pwd));
assert.eq(1, rst2AdminDB.auth(kSystemUser.name, kSystemUser.pwd));

const rst2KeyDoc = rst2AdminDB.system.keys.findOne({_id: rst2ClusterTime.signature.keyId});
assert.commandWorked(rst1AdminDB.system.external_validation_keys.insert({
    keyId: rst2KeyDoc._id,
    purpose: rst2KeyDoc.purpose,
    key: rst2KeyDoc.key,
    expiresAt: rst2KeyDoc.expiresAt,
    replicaSetName: rst2.name,
},
                                                                        {w: "majority"}));

rst1AdminDB.logout();
rst2AdminDB.logout();

jsTest.log("Verify that rst1 can now validate the clusterTime from rst2 using the key doc in " +
           "admin.system.external_validation_keys");
assert.eq(1, rst1TestDB.auth(kTestUser.name, kTestUser.pwd));
assert.commandWorked(rst1TestDB.runCommand({find: kCollName, $clusterTime: rst2ClusterTime}));

rst1.stopSet();
rst2.stopSet();
})();
