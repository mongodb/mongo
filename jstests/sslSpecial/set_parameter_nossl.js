// Test changing the --sslMode and --clusterAuthMode 
// parameters using setParameter

port = allocatePorts(1)[0];

// setParameter should always fail since it
// cannot be used to transition from disabled/keyFile modes
function testTransition(newSSLMode, newClusterAuthMode) {
    // If no parameters are given sslMode defaults to disabled
    var conn = MongoRunner.runMongod({port: port,
                                      clusterAuthMode: "keyFile"})
    var adminDB = conn.getDB("admin"); 
    var res = adminDB.runCommand({ "setParameter" : 1,
                                   "sslMode" : newSSLMode });
    assert(!res["ok"]);

    var res = adminDB.runCommand({ "setParameter" : 1,
                                   "clusterAuthMode" : newClusterAuthMode });
    assert(!res["ok"]);
    MongoRunner.stopMongod(port);
}

testTransition("allowSSL", "sendKeyFile");
testTransition("preferSSL", "sendX509");
testTransition("requireSSL", "x509");

