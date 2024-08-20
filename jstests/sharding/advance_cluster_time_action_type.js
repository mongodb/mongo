/**
 * Test a role with an advanceClusterTime action type.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

// Binaries earlier than 8.0 fail to parse a missing signature.
// TODO SERVER-88458: Remove.
const canParseMissingClusterTimeSignature =
    MongoRunner.compareBinVersions(jsTestOptions().mongosBinVersion, "8.0") >= 0;

let st = new ShardingTest({
    mongos: 1,
    shards: 1,
    keyFile: 'jstests/libs/key1',
    rs: {setParameter: {"failpoint.alwaysValidateClientsClusterTime": tojson({mode: "alwaysOn"})}}
});

let adminDB = st.s.getDB('admin');

assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
assert.eq(1, adminDB.auth("admin", "admin"));

assert.commandWorked(adminDB.runCommand({
    createRole: "advanceClusterTimeRole",
    privileges: [{resource: {cluster: true}, actions: ["advanceClusterTime"]}],
    roles: []
}));

let testDB = adminDB.getSiblingDB("testDB");

assert.commandWorked(
    testDB.runCommand({createUser: 'NotTrusted', pwd: 'pwd', roles: ['readWrite']}));
assert.commandWorked(testDB.runCommand({
    createUser: 'Trusted',
    pwd: 'pwd',
    roles: [{role: 'advanceClusterTimeRole', db: 'admin'}, 'readWrite']
}));
adminDB.logout();
assert.eq(1, testDB.auth("NotTrusted", "pwd"));

let res = testDB.runCommand({insert: "foo", documents: [{_id: 0}]});
assert.commandWorked(res);

let clusterTime = Object.assign({}, res.$clusterTime);
let clusterTimeTS = new Timestamp(clusterTime.clusterTime.getTime() + 1000, 0);
clusterTime.clusterTime = clusterTimeTS;
assert(clusterTime.hasOwnProperty("signature"), tojson(clusterTime));

const cmdObj = {
    find: "foo",
    limit: 1,
    singleBatch: true,
    $clusterTime: clusterTime
};
jsTestLog("running NonTrusted. command: " + tojson(cmdObj));
res = testDB.runCommand(cmdObj);
assert.commandFailedWithCode(
    res, ErrorCodes.TimeProofMismatch, "Command request was: " + tojsononeline(cmdObj));

// Make a copy so anything with a reference to the original cluster time isn't affected.
let clusterTimeCopy = Object.extend({}, clusterTime);
delete clusterTimeCopy.signature;
assert(!clusterTimeCopy.hasOwnProperty("signature"), tojson(clusterTimeCopy));
assert(clusterTime.hasOwnProperty("signature"), tojson(clusterTime));
const cmdObjNoSignature = {
    find: "foo",
    limit: 1,
    singleBatch: true,
    $clusterTime: clusterTimeCopy
};
jsTestLog("running NonTrusted. command: " + tojson(cmdObjNoSignature));
res = testDB.runCommand(cmdObjNoSignature);
if (canParseMissingClusterTimeSignature) {
    assert.commandFailedWithCode(
        res, ErrorCodes.KeyNotFound, "Command request was: " + tojsononeline(cmdObjNoSignature));
} else {
    // Parsing will fail.
    assert.commandFailedWithCode(
        res, ErrorCodes.NoSuchKey, "Command request was: " + tojsononeline(cmdObjNoSignature));
}

testDB.logout();

assert.eq(1, testDB.auth("Trusted", "pwd"));
jsTestLog("running Trusted. command: " + tojson(cmdObj));
res = testDB.runCommand(cmdObj);
assert.commandWorked(res, "Command request was: " + tojsononeline(cmdObj));

// Verify we can advance without a signature if we have the advanceClusterTime privilege.

// Make a copy so anything with a reference to the original cluster time isn't affected.
clusterTimeCopy = Object.extend({}, res.$clusterTime);
clusterTimeTS = new Timestamp(clusterTimeCopy.clusterTime.getTime() + 1000, 0);
clusterTimeCopy.clusterTime = clusterTimeTS;
delete clusterTimeCopy.signature;
assert(!clusterTimeCopy.hasOwnProperty("signature"), tojson(clusterTimeCopy));
assert(res.$clusterTime.hasOwnProperty("signature"), tojson(clusterTime));

cmdObjNoSignature.$clusterTime = clusterTimeCopy;

jsTestLog("running Trusted. command: " + tojson(cmdObjNoSignature));
res = testDB.runCommand(cmdObjNoSignature);
if (canParseMissingClusterTimeSignature) {
    assert.commandWorked(res, "Command request was: " + tojsononeline(cmdObjNoSignature));
} else {
    // Parsing will fail.
    assert.commandFailedWithCode(
        res, ErrorCodes.NoSuchKey, "Command request was: " + tojsononeline(cmdObjNoSignature));
}

testDB.logout();

st.stop();
