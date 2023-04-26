// Test setParameter sslCipherSuitesConfig for TLS 1.3
// sslCipherSuitesConfig allows the user to set the list of cipher suites for just TLS 1.3

(function() {
"use strict";
load("jstests/ssl/libs/ssl_helpers.js");

// Short circuits for system configurations that do not support this setParameter, (i.e. OpenSSL
// that don't support TLS 1.3)
if (determineSSLProvider() !== "openssl") {
    jsTestLog("SSL provider is not OpenSSL; skipping test.");
    return;
} else if (detectDefaultTLSProtocol() !== "TLS1_3") {
    jsTestLog("Platform does not support TLS 1.3; skipping test.");
    return;
}

const baseParams = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/ssl/x509/root-and-trusted-ca.pem",
    waitForConnect: false,
};

function testConn() {
    const mongo = runMongoProgram('mongo',
                                  '--host',
                                  'localhost',
                                  '--port',
                                  mongod.port,
                                  '--tls',
                                  '--tlsCAFile',
                                  'jstests/libs/ca.pem',
                                  '--tlsCertificateKeyFile',
                                  'jstests/libs/trusted-client.pem',
                                  '--eval',
                                  ';');
    return mongo === 0;
}

// test a successful connection when setting cipher suites
jsTestLog("Testing for successful connection with valid cipher suite config");
let mongod = MongoRunner.runMongod(
    Object.merge(baseParams, {setParameter: {opensslCipherSuiteConfig: "TLS_AES_256_GCM_SHA384"}}));
assert.soon(testConn, "Client could not connect to server with valid ciphersuite config.");
MongoRunner.stopMongod(mongod);

// test an unsuccessful connection when mandating a cipher suite which OpenSSL disables by default
jsTestLog(
    "Testing for unsuccessful connection with cipher suite config which OpenSSL disables by default.");
mongod = MongoRunner.runMongod(Object.merge(
    baseParams, {setParameter: {opensslCipherSuiteConfig: "TLS_AES_128_CCM_8_SHA256"}}));
sleep(30000);

assert.eq(
    false, testConn(), "Client successfully connected to server with invalid ciphersuite config.");
MongoRunner.stopMongod(mongod);
})();
