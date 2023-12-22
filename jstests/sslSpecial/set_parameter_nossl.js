// Test changing the --tlsMode and --clusterAuthMode
// parameters using setParameter

// setParameter should always fail since it
// cannot be used to transition from disabled/keyFile modes
function testTransition(newtlsMode, newClusterAuthMode) {
    // If no parameters are given tlsMode defaults to disabled
    var conn = MongoRunner.runMongod({clusterAuthMode: "keyFile", keyFile: 'jstests/libs/key1'});
    var adminDB = conn.getDB("admin");
    adminDB.createUser({user: "root", pwd: "pwd", roles: ["root"]});
    adminDB.auth("root", "pwd");
    var res = adminDB.runCommand({"setParameter": 1, "tlsMode": newtlsMode});
    assert.commandFailedWithCode(res, ErrorCodes.BadValue);

    res = adminDB.runCommand({"setParameter": 1, "clusterAuthMode": newClusterAuthMode});
    assert.commandFailedWithCode(res, ErrorCodes.BadValue);
    MongoRunner.stopMongod(conn);
}

testTransition("allowTLS", "sendKeyFile");
testTransition("preferTLS", "sendX509");
testTransition("requireTLS", "x509");
