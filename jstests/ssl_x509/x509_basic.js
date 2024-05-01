// Check that if a hello command for intracluster auth contains both the saslSupportedMechs field
// for the __system user and a speculativeAuthenticate field for X509, we do not see a log marking
// the changing of the username from __system to that specified in the x509 certificate.
// @tags: [
//   disables_test_commands,
// ]
import {findMatchingLogLine} from "jstests/libs/log.js";

const x509_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    tlsClusterFile: "jstests/libs/cluster_cert.pem",
    tlsAllowInvalidHostnames: "",
    clusterAuthMode: "x509"
};
const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

function runTest() {
    const mongo = MongoRunner.runMongod(Object.merge(x509_options, {auth: ""}));

    const ext = mongo.getDB("$external");
    ext.createUser({user: CLIENT_USER, roles: []});

    assert.commandWorked(ext.runCommand({
        hello: 1,
        saslSupportedMechs: "local.__system",
        speculativeAuthenticate: {authenticate: "1", mechanism: "MONGODB-X509", db: "$external"}
    }));

    const profileLevelDB = mongo.getDB("x509_basic");
    const globalLog = assert.commandWorked(profileLevelDB.adminCommand({getLog: 'global'}));
    const fieldMatcher = {msg: "Different user name was supplied to saslSupportedMechs"};
    assert.eq(
        null,
        findMatchingLogLine(globalLog.log, fieldMatcher),
        "Found log line concerning \"Different user name was supplied to saslSupportedMechs\" when we did not expect to.");
    MongoRunner.stopMongod(mongo);
}

// Since the logic around this log has special-casing around enableTestCommands, assert that the log
// is absent both when in test-mode and normal mode.
TestData.enableTestCommands = true;
runTest();
TestData.enableTestCommands = false;
runTest();
