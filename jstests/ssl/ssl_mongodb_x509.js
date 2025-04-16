import {ReplSetTest} from "jstests/libs/replsettest.js";
import {SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

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
assert.neq(-1,
           rawMongoProgramOutput(".*").search("No TLS certificate validation can be performed"));
configRS.stopSet();
