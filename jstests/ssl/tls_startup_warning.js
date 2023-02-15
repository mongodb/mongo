// Test for startup warning when TLS auth and certificates are added

(function() {
'use strict';

const SERVER_CERT = 'jstests/libs/server.pem';
const COMBINED_CA_CERT = 'jstests/ssl/x509/root-and-trusted-ca.pem';
const CLUSTER_CA_CERT = "jstests/libs/ca.pem";

function runTest(opts, expectWarningCertifcates) {
    clearRawMongoProgramOutput();
    let mongo = MongoRunner.runMongod(Object.assign({
        auth: '',
        waitForConnect: false,
        tlsMode: 'requireTLS',
    },
                                                    opts));

    // Allow time for output to generate before verifying log is not included.
    if (!expectWarningCertifcates) {
        sleep(2000);
    }
    assert.soon(function() {
        const output = rawMongoProgramOutput();
        return (
            expectWarningCertifcates ==
            output.includes(
                'No client certificate validation can be performed since no CA file or cluster CA File has been provided. Please specify an sslCAFile or a clusterCAFile parameter'));
    });

    stopMongoProgramByPid(mongo.pid);
}
// Warning should not be shown since CA File is included.
runTest({tlsCertificateKeyFile: SERVER_CERT, tlsCAFile: COMBINED_CA_CERT}, false);

// Warning should not be shown since Cluster CA File is included.
runTest({tlsCertificateKeyFile: SERVER_CERT, tlsClusterCAFile: CLUSTER_CA_CERT}, false);

// Warning should show since neither cert is included.
runTest({tlsCertificateKeyFile: SERVER_CERT}, true);
})();
