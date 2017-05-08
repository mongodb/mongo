// Test changing the --sslMode and --clusterAuthMode
// parameters using setParameter

// setParameter should always fail since it
// cannot be used to transition from disabled/keyFile modes
function testTransition(newSSLMode, newClusterAuthMode) {
    // If no parameters are given sslMode defaults to disabled
    var conn = MongoRunner.runMongod({clusterAuthMode: "keyFile"});
    var adminDB = conn.getDB("admin");
    adminDB.createUser({user: "root", pwd: "pwd", roles: ["root"]});
    adminDB.auth("root", "pwd");
    var res = adminDB.runCommand({"setParameter": 1, "sslMode": newSSLMode});
    assert.commandFailedWithCode(res, ErrorCodes.BadValue);

    var res = adminDB.runCommand({"setParameter": 1, "clusterAuthMode": newClusterAuthMode});
    assert.commandFailedWithCode(res, ErrorCodes.BadValue);
    MongoRunner.stopMongod(conn);
}

testTransition("allowSSL", "sendKeyFile");
testTransition("preferSSL", "sendX509");
testTransition("requireSSL", "x509");
