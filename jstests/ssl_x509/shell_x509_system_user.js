// Check that the shell can authenticate as the __system user using X509, which is a use case for
// our auth performance tests (through the dbhash hook).

import {ReplSetTest} from "jstests/libs/replsettest.js";

const x509Options = {
    clusterAuthMode: 'x509',
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: 'jstests/libs/server.pem',
    tlsCAFile: 'jstests/libs/ca.pem',
    tlsAllowInvalidCertificates: '',
};

const rst = new ReplSetTest({nodes: 1, nodeOptions: x509Options, waitForKeys: false});

rst.startSet();

rst.initiate();

const primaryConnString = rst.getPrimary().host;

const subShellCommands = async function() {
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
    new ReplSetTest(db.getMongo().host);

    // Directly check that the use case for our auth perf tests can succeed.
    await import("jstests/hooks/run_check_repl_dbhash.js");
};

const subShellArgs = [
    'mongo',
    '--ssl',
    '--tlsCAFile=jstests/libs/ca.pem',
    '--tlsCertificateKeyFile=jstests/libs/server.pem',
    '--tlsAllowInvalidHostnames',
    '--authenticationDatabase=$external',
    '--authenticationMechanism=MONGODB-X509',
    primaryConnString,
    '--eval',
    `(${subShellCommands.toString()})();`
];

const retVal = _runMongoProgram(...subShellArgs);
assert.eq(retVal, 0, 'mongo shell did not succeed with exit code 0');

rst.stopSet();
