var SERVER_CERT = "jstests/libs/server.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var CLIENT_USER = "C=US,ST=New York,L=New York City,O=BongoDB,OU=KernelUser,CN=client";

jsTest.log("Assert x509 auth is not allowed when a standalone bongod is run without a CA file.");

// allowSSL instead of requireSSL so that the non-SSL connection succeeds.
var conn = BongoRunner.runBongod({sslMode: 'allowSSL', sslPEMKeyFile: SERVER_CERT, auth: ''});

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
var exitStatus = runBongoProgram('bongo',
                                 '--ssl',
                                 '--sslAllowInvalidCertificates',
                                 '--sslPEMKeyFile',
                                 CLIENT_CERT,
                                 '--port',
                                 conn.port,
                                 '--eval',
                                 ('quit(db.getSisterDB("$external").auth({' +
                                  'user: "' + CLIENT_USER + '" ,' +
                                  'mechanism: "BONGODB-X509"}));'));

assert.eq(exitStatus, 0, "authentication via BONGODB-X509 without CA succeeded");

BongoRunner.stopBongod(conn.port);

jsTest.log("Assert bongod doesn\'t start with CA file missing and clusterAuthMode=x509.");

var sslParams = {clusterAuthMode: 'x509', sslMode: 'requireSSL', sslPEMKeyFile: SERVER_CERT};
var conn = BongoRunner.runBongod(sslParams);
assert.isnull(conn, "server started with x509 clusterAuthMode but no CA file");

jsTest.log("Assert bongos doesn\'t start with CA file missing and clusterAuthMode=x509.");

assert.throws(function() {
    new ShardingTest({
        shards: 1,
        bongos: 1,
        verbose: 2,
        other: {configOptions: sslParams, bongosOptions: sslParams, shardOptions: sslParams}
    });
}, [], "bongos started with x509 clusterAuthMode but no CA file");
