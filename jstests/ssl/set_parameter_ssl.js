// Test changing the --sslMode and --clusterAuthMode 
// parameters using setParameter
TestData.useX509 = false;

var SERVER_CERT = "jstests/libs/server.pem"
var CA_CERT = "jstests/libs/ca.pem" 
port = allocatePorts(1)[0];

function testSSLTransition(oldMode, newMode, shouldSucceed) {
    var conn = MongoRunner.runMongod({port: port,
                           sslMode: oldMode, 
                           sslPEMKeyFile: SERVER_CERT,
                           sslCAFile: CA_CERT});
    
    var adminDB = conn.getDB("admin"); 
    var res = adminDB.runCommand({ "setParameter" : 1,
                                   "sslMode" : newMode });

    assert(res["ok"] == shouldSucceed);
    stopMongod(port);
}

function testAuthModeTransition(oldMode, newMode, shouldSucceed) {
    var conn = MongoRunner.runMongod({port: port,
                           sslMode: "requireSSL", 
                           sslPEMKeyFile: SERVER_CERT,
                           sslCAFile: CA_CERT,
                           clusterAuthMode: oldMode});
    
    var adminDB = conn.getDB("admin"); 
    var res = adminDB.runCommand({ "setParameter" : 1,
                                   "clusterAuthMode" : newMode });

    assert(res["ok"] == shouldSucceed);
    stopMongod(port);
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

testAuthModeTransition("sendKeyFile", "invalid", false);
testAuthModeTransition("sendKeyFile", "keyFile", false);
testAuthModeTransition("sendKeyFile", "sendKeyFile", false);
testAuthModeTransition("sendKeyFile", "sendX509", true);
testAuthModeTransition("sendKeyFile", "x509", false);
testAuthModeTransition("sendX509", "invalid", false);
testAuthModeTransition("sendX509", "keyFile", false);
testAuthModeTransition("sendX509", "sendKeyFile", false);
testAuthModeTransition("sendX509", "sendX509", false);
testAuthModeTransition("sendX509", "x509", true);
testAuthModeTransition("x509", "invalid", false);
testAuthModeTransition("x509", "keyFile", false);
testAuthModeTransition("x509", "sendKeyFile", false);
testAuthModeTransition("x509", "sendX509", false);
testAuthModeTransition("x509", "x509", false);
