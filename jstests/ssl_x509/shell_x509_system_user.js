// Check that the shell can authenticate as the __system user using X509, which is a use case for
// our auth performance tests (through the dbhash hook).

(function() {
'use strict';

// The mongo shell cannot authenticate as the internal __system user in tests that use x509 for
// cluster authentication. Choosing the default value for wcMajorityJournalDefault in
// ReplSetTest cannot be done automatically without the shell performing such authentication, so
// in this test we must make the choice explicitly, based on the global test options.
let wcMajorityJournalDefault;
if (jsTestOptions().storageEngine == "inMemory") {
    wcMajorityJournalDefault = false;
} else {
    wcMajorityJournalDefault = true;
}

const x509Options = {
    clusterAuthMode: 'x509',
    sslMode: 'requireSSL',
    sslPEMKeyFile: 'jstests/libs/server.pem',
    sslCAFile: 'jstests/libs/ca.pem',
    sslAllowInvalidCertificates: '',
};

const rst = new ReplSetTest({nodes: 1, nodeOptions: x509Options, waitForKeys: false});

rst.startSet();

rst.initiate();

const primaryConnString = rst.getPrimary().host;

const subShellCommands = function() {
    TestData = {
        authUser: 'C=US,ST=New York,L=New York City,O=MongoDB,OU=Kernel,CN=server',
        authenticationDatabase: '$external',
        keyFile: 'dummyKeyFile',
        clusterAuthMode: 'x509',

    };
    // Explicitly check asCluster can succeed.
    authutil.asCluster(db.getMongo(), 'dummyKeyFile', function() {
        // No need to do anything here. We just need to check we don't error out in the
        // previous auth step.
    });

    // Indirectly check that ReplSetTest can successfully call asCluster.
    const rst = new ReplSetTest(db.getMongo().host);

    // Directly check that the use case for our auth perf tests can succeed.
    load("jstests/hooks/run_check_repl_dbhash.js");
};

const subShellArgs = [
    'mongo',
    '--ssl',
    '--sslCAFile=jstests/libs/ca.pem',
    '--sslPEMKeyFile=jstests/libs/server.pem',
    '--sslAllowInvalidHostnames',
    '--authenticationDatabase=$external',
    '--authenticationMechanism=MONGODB-X509',
    primaryConnString,
    '--eval',
    `(${subShellCommands.toString()})();`
];

const retVal = _runMongoProgram(...subShellArgs);
assert.eq(retVal, 0, 'mongo shell did not succeed with exit code 0');

rst.stopSet();
})();
