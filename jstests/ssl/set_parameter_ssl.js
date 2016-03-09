// Test changing the --sslMode and --clusterAuthMode parameters using setParameter

var SERVER_CERT = "jstests/libs/server.pem";
var CA_CERT = "jstests/libs/ca.pem";

function testSSLTransition(oldMode, newMode, shouldSucceed) {
    var conn =
        MongoRunner.runMongod({sslMode: oldMode, sslPEMKeyFile: SERVER_CERT, sslCAFile: CA_CERT});

    var adminDB = conn.getDB("admin");
    adminDB.createUser({user: "root", pwd: "pwd", roles: ['root']});
    adminDB.auth("root", "pwd");
    var res = adminDB.runCommand({"setParameter": 1, "sslMode": newMode});

    assert(res["ok"] == shouldSucceed, tojson(res));
    MongoRunner.stopMongod(conn.port);
}

function testAuthModeTransition(oldMode, newMode, sslMode, shouldSucceed) {
    var conn = MongoRunner.runMongod({
        sslMode: sslMode,
        sslPEMKeyFile: SERVER_CERT,
        sslCAFile: CA_CERT,
        clusterAuthMode: oldMode
    });

    var adminDB = conn.getDB("admin");
    adminDB.createUser({user: "root", pwd: "pwd", roles: ['root']});
    adminDB.auth("root", "pwd");
    var res = adminDB.runCommand({"setParameter": 1, "clusterAuthMode": newMode});

    assert(res["ok"] == shouldSucceed, tojson(res));
    MongoRunner.stopMongod(conn.port);
}

testSSLTransition("allowSSL", "invalid", false);
testSSLTransition("allowSSL", "disabled", false);
testSSLTransition("allowSSL", "allowSSL", false);
testSSLTransition("allowSSL", "preferSSL", true);
testSSLTransition("allowSSL", "requireSSL", false);
testSSLTransition("preferSSL", "invalid", false);
testSSLTransition("preferSSL", "disabled", false);
testSSLTransition("preferSSL", "allowSSL", false);
testSSLTransition("preferSSL", "preferSSL", false);
testSSLTransition("preferSSL", "requireSSL", true);
testSSLTransition("requireSSL", "invalid", false);
testSSLTransition("requireSSL", "disabled", false);
testSSLTransition("requireSSL", "allowSSL", false);
testSSLTransition("requireSSL", "preferSSL", false);
testSSLTransition("requireSSL", "requireSSL", false);

testAuthModeTransition("sendKeyFile", "invalid", "requireSSL", false);
testAuthModeTransition("sendKeyFile", "keyFile", "requireSSL", false);
testAuthModeTransition("sendKeyFile", "sendKeyFile", "requireSSL", false);
testAuthModeTransition("sendKeyFile", "sendX509", "requireSSL", true);
testAuthModeTransition("sendKeyFile", "x509", "requireSSL", false);
testAuthModeTransition("sendX509", "invalid", "requireSSL", false);
testAuthModeTransition("sendX509", "keyFile", "requireSSL", false);
testAuthModeTransition("sendX509", "sendKeyFile", "requireSSL", false);
testAuthModeTransition("sendX509", "sendX509", "requireSSL", false);
testAuthModeTransition("sendX509", "x509", "requireSSL", true);
testAuthModeTransition("x509", "invalid", "requireSSL", false);
testAuthModeTransition("x509", "keyFile", "requireSSL", false);
testAuthModeTransition("x509", "sendKeyFile", "requireSSL", false);
testAuthModeTransition("x509", "sendX509", "requireSSL", false);
testAuthModeTransition("x509", "x509", "requireSSL", false);

testAuthModeTransition("sendKeyFile", "invalid", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "keyFile", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "sendKeyFile", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "sendX509", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "x509", "allowSSL", false);
