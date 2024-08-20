import {ReplSetTest} from "jstests/libs/replsettest.js";

var SERVER_CERT = "jstests/libs/server.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var CLIENT_USER = "C=US,ST=New York,L=New York City,O=MongoDB,OU=KernelUser,CN=client";

jsTest.log("Assert x509 auth is not allowed when a standalone mongod is run without a CA file.");

// allowTLS instead of requireTLS so that the non-SSL connection succeeds.
var conn = MongoRunner.runMongod({
    tlsMode: 'allowTLS',
    tlsCertificateKeyFile: SERVER_CERT,
    auth: '',
    tlsCAFile: 'jstests/libs/ca.pem'
});

var external = conn.getDB('$external');
external.createUser({
    user: CLIENT_USER,
    roles: [
        {'role': 'userAdminAnyDatabase', 'db': 'admin'},
        {'role': 'readWriteAnyDatabase', 'db': 'admin'}
    ]
});

// Should not be able to authenticate with x509.
// Authenticate call will return 1 on success, 0 on error.
var exitStatus = runMongoProgram('mongo',
                                 '--tls',
                                 '--tlsAllowInvalidCertificates',
                                 '--tlsCertificateKeyFile',
                                 CLIENT_CERT,
                                 '--port',
                                 conn.port,
                                 '--eval',
                                 ('quit(db.getSiblingDB("$external").auth({' +
                                  'user: "' + CLIENT_USER + '" ,' +
                                  'mechanism: "MONGODB-X509"}));'));

jsTest.log("exitStatus: " + exitStatus);

assert.eq(exitStatus, 0, "authentication via MONGODB-X509 without CA succeeded");

MongoRunner.stopMongod(conn);

jsTest.log("Assert mongod doesn\'t start with CA file missing and clusterAuthMode=x509.");

var tlsParams = {
    clusterAuthMode: 'x509',
    tlsMode: 'requireTLS',
    setParameter: {tlsUseSystemCA: true},
    tlsCertificateKeyFile: SERVER_CERT
};
assert.throws(() => MongoRunner.runMongod(tlsParams),
              [],
              "server started with x509 clusterAuthMode but no CA file");

jsTest.log("Assert mongos doesn\'t start with CA file missing and clusterAuthMode=x509.");

var rstOptions = {
    waitForKeys: false,
    isConfigServer: true,
    hostname: getHostName(),
    useHostName: true,
    nodes: 1
};
var startOptions = {
    // Ensure that journaling is always enabled for config servers.
    configsvr: "",
    storageEngine: "wiredTiger",
    tlsMode: 'allowTLS',
    tlsCertificateKeyFile: 'jstests/libs/trusted-server.pem',
    tlsCAFile: 'jstests/libs/ca.pem'
};

var configRS = new ReplSetTest(rstOptions);

configRS.startSet(startOptions);

// Make sure the mongoS failed to start up for the proper reason.
assert.throws(() => MongoRunner.runMongos({
    clusterAuthMode: 'x509',
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: SERVER_CERT,
    configdb: configRS.getURL()
}),
              [],
              "mongos started with x509 clusterAuthMode but no CA file");
assert.neq(-1, rawMongoProgramOutput().search("No TLS certificate validation can be performed"));
configRS.stopSet();
