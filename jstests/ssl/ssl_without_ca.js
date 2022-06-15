var SERVER_CERT = "jstests/libs/server.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var CLIENT_USER = "C=US,ST=New York,L=New York City,O=MongoDB,OU=KernelUser,CN=client";

jsTest.log("Assert x509 auth is not allowed when a standalone mongod is run without a CA file.");

// allowSSL instead of requireSSL so that the non-SSL connection succeeds.
var conn = MongoRunner.runMongod({sslMode: 'allowSSL', sslPEMKeyFile: SERVER_CERT, auth: ''});

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
                                 '--ssl',
                                 '--sslAllowInvalidCertificates',
                                 '--sslPEMKeyFile',
                                 CLIENT_CERT,
                                 '--port',
                                 conn.port,
                                 '--eval',
                                 ('quit(db.getSiblingDB("$external").auth({' +
                                  'user: "' + CLIENT_USER + '" ,' +
                                  'mechanism: "MONGODB-X509"}));'));

assert.eq(exitStatus, 0, "authentication via MONGODB-X509 without CA succeeded");

MongoRunner.stopMongod(conn);

jsTest.log("Assert mongod doesn\'t start with CA file missing and clusterAuthMode=x509.");

var sslParams = {clusterAuthMode: 'x509', sslMode: 'requireSSL', sslPEMKeyFile: SERVER_CERT};
assert.throws(() => MongoRunner.runMongod(sslParams),
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
    sslMode: 'allowSSL',
    sslPEMKeyFile: 'jstests/libs/trusted-server.pem'
};

var configRS = new ReplSetTest(rstOptions);
configRS.startSet(startOptions);

// Make sure the mongoS failed to start up for the proper reason.
assert.throws(() => MongoRunner.runMongos({
    clusterAuthMode: 'x509',
    sslMode: 'requireSSL',
    sslPEMKeyFile: SERVER_CERT,
    configdb: configRS.getURL()
}),
              [],
              "mongos started with x509 clusterAuthMode but no CA file");
assert.neq(-1, rawMongoProgramOutput().search("No TLS certificate validation can be performed"));
configRS.stopSet();
