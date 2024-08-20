/**
 * Test that the client cannot gossip clusterTime from one replica set to another if it doesn't
 * have the advanceClusterTime privilege.
 *
 * @tags: [
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

const kDbName = "testDb";
const kCollName = "testColl";

const kAdminUser = {
    name: "admin",
    pwd: "admin",
};

const kUserWithoutAdvanceClusterTimePrivilege = {
    name: "cannotAdvanceClusterTime",
    pwd: "pwd"
};
const kUserWithAdvanceClusterTimePrivilege = {
    name: "canAdvanceClusterTime",
    pwd: "pwd"
};

function createUsers(primary) {
    // Create the admin user and advanceClusterTimeRole.
    const adminDB = primary.getDB("admin");
    assert.commandWorked(
        adminDB.runCommand({createUser: kAdminUser.name, pwd: kAdminUser.pwd, roles: ["root"]}));
    assert.eq(1, adminDB.auth(kAdminUser.name, kAdminUser.pwd));
    assert.commandWorked(adminDB.runCommand({
        createRole: "advanceClusterTimeRole",
        privileges: [{resource: {cluster: true}, actions: ["advanceClusterTime"]}],
        roles: []
    }));

    // Create one user without advanceClusterTime privilege, and one with advanceClusterTime
    // privilege.
    const testDB = primary.getDB(kDbName);
    assert.commandWorked(testDB.runCommand({
        createUser: kUserWithoutAdvanceClusterTimePrivilege.name,
        pwd: kUserWithoutAdvanceClusterTimePrivilege.pwd,
        roles: ["readWrite"]
    }));
    assert.commandWorked(testDB.runCommand({
        createUser: kUserWithAdvanceClusterTimePrivilege.name,
        pwd: kUserWithAdvanceClusterTimePrivilege.pwd,
        roles: [{role: "advanceClusterTimeRole", db: "admin"}, "readWrite"]
    }));
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

// Binaries earlier than 8.0 fail to parse a missing signature.
// TODO SERVER-88458: Remove.
const canAlwaysParseMissingClusterTimeSignature =
    !jsTest.options().useRandomBinVersionsWithinReplicaSet;

createUsers(rst1Primary);
createUsers(rst2Primary);

const rst1TestDB = rst1Primary.getDB(kDbName);
const rst2TestDB = rst2Primary.getDB(kDbName);

// Test clusterTime gossip when the client does not have advanceClusterTime privilege.
(() => {
    assert.eq(1,
              rst1TestDB.auth(kUserWithoutAdvanceClusterTimePrivilege.name,
                              kUserWithoutAdvanceClusterTimePrivilege.pwd));
    assert.eq(1,
              rst2TestDB.auth(kUserWithoutAdvanceClusterTimePrivilege.name,
                              kUserWithoutAdvanceClusterTimePrivilege.pwd));

    const rst1ClusterTime =
        assert.commandWorked(rst1TestDB.runCommand({find: kCollName})).$clusterTime;
    const rst2ClusterTime =
        assert.commandWorked(rst2TestDB.runCommand({insert: kCollName, documents: [{_id: 0}]}))
            .$clusterTime;
    jsTest.log("rst1's clusterTime " + tojson(rst1ClusterTime));
    jsTest.log("rst2's clusterTime " + tojson(rst2ClusterTime));

    // External clients always receive cluster time signatures.
    assert(rst1ClusterTime.hasOwnProperty("clusterTime") &&
               rst1ClusterTime.hasOwnProperty("signature"),
           tojson(rst1ClusterTime));
    assert(rst2ClusterTime.hasOwnProperty("clusterTime") &&
               rst2ClusterTime.hasOwnProperty("signature"),
           tojson(rst2ClusterTime));

    // A key's keyId is generated as the node's current clusterTime's Timestamp so it is possible
    // for the keyId for rst2ClusterTime to match the key on rst1, and in that case the command
    // would fail with TimeProofMismatch instead of KeyNotFound.
    assert.commandFailedWithCode(
        rst1TestDB.runCommand({find: kCollName, $clusterTime: rst2ClusterTime}),
        [ErrorCodes.TimeProofMismatch, ErrorCodes.KeyNotFound]);

    // Sending no signature without the advanceClusterTime privilege fails because this is treated
    // the same as sending the dummy signature with keyId 0.

    // Make a copy so anything with a reference to the original cluster time isn't affected.
    let clusterTimeCopy = Object.extend({}, rst2ClusterTime);
    delete clusterTimeCopy.signature;
    assert(!clusterTimeCopy.hasOwnProperty("signature"), tojson(clusterTimeCopy));
    assert(rst2ClusterTime.hasOwnProperty("signature"), tojson(rst2ClusterTime));

    if (canAlwaysParseMissingClusterTimeSignature) {
        assert.commandFailedWithCode(
            rst1TestDB.runCommand({find: kCollName, $clusterTime: clusterTimeCopy}),
            ErrorCodes.KeyNotFound);
    } else {
        // We test with mixed version replica sets. An 8.0 binary primary can parse the signature
        // and will fail like above, but a lower binary will fail to parse.
        assert.commandFailedWithCode(
            rst1TestDB.runCommand({find: kCollName, $clusterTime: clusterTimeCopy}),
            [ErrorCodes.NoSuchKey, ErrorCodes.KeyNotFound]);
    }

    rst1TestDB.logout();
    rst2TestDB.logout();
})();

// Test clusterTime gossip when the client does have advanceClusterTime privilege.
(() => {
    assert.eq(1,
              rst1TestDB.auth(kUserWithAdvanceClusterTimePrivilege.name,
                              kUserWithAdvanceClusterTimePrivilege.pwd));
    assert.eq(1,
              rst2TestDB.auth(kUserWithoutAdvanceClusterTimePrivilege.name,
                              kUserWithoutAdvanceClusterTimePrivilege.pwd));

    const rst1ClusterTime =
        assert.commandWorked(rst1TestDB.runCommand({find: kCollName})).$clusterTime;
    const rst2ClusterTime =
        assert.commandWorked(rst2TestDB.runCommand({insert: kCollName, documents: [{_id: 1}]}))
            .$clusterTime;
    jsTest.log("rst1's clusterTime " + tojson(rst1ClusterTime));
    jsTest.log("rst2's clusterTime " + tojson(rst2ClusterTime));

    // External clients always receive cluster time signatures.
    assert(rst1ClusterTime.hasOwnProperty("clusterTime") &&
               rst1ClusterTime.hasOwnProperty("signature"),
           tojson(rst1ClusterTime));
    assert(rst2ClusterTime.hasOwnProperty("clusterTime") &&
               rst2ClusterTime.hasOwnProperty("signature"),
           tojson(rst2ClusterTime));

    assert.commandWorked(rst1TestDB.runCommand({find: kCollName, $clusterTime: rst2ClusterTime}));

    // Sending no signature without the advanceClusterTime privilege fails because this is treated
    // the same as sending the dummy signature with keyId 0.

    // Make a copy so anything with a reference to the original cluster time isn't affected.
    let clusterTimeCopy = Object.extend({}, rst2ClusterTime);
    delete clusterTimeCopy.signature;
    assert(!clusterTimeCopy.hasOwnProperty("signature"), tojson(clusterTimeCopy));
    assert(rst2ClusterTime.hasOwnProperty("signature"), tojson(rst2ClusterTime));

    if (canAlwaysParseMissingClusterTimeSignature) {
        assert.commandWorked(
            rst1TestDB.runCommand({find: kCollName, $clusterTime: clusterTimeCopy}));
    } else {
        // We test with mixed version replica sets. An 8.0 binary primary can parse the signature
        // and will succeed like above, but a lower binary will fail to parse.
        assert.commandWorkedOrFailedWithCode(
            rst1TestDB.runCommand({find: kCollName, $clusterTime: clusterTimeCopy}),
            ErrorCodes.NoSuchKey);
    }

    rst1TestDB.logout();
    rst2TestDB.logout();
})();

rst1.stopSet();
rst2.stopSet();
