/**
 * Tests that the server can load its keys and certificates from the certificate store
 * through a selector, and that the correct keys are used for ingress/egress connections.
 */

import {getPython3Binary} from "jstests/libs/python.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    requireSSLProvider,
    TRUSTED_CA_CERT,
    TRUSTED_CLIENT_CERT,
    TRUSTED_SERVER_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";

const clientThumbprint = cat('jstests/libs/trusted-client.pem.digest.sha1');
const serverThumbprint = cat('jstests/libs/trusted-server.pem.digest.sha1');
const CLIENT = 'CN=Trusted Kernel Test Client,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US';
const SERVER = 'CN=Trusted Kernel Test Server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US';

const testCases = [
    {
        selector: `thumbprint=${serverThumbprint}`,
        expectIngressKeyUsed: SERVER,
        expectEgressKeyUsed: SERVER
    },
    {
        selector: `thumbprint=${serverThumbprint}`,
        clusterSelector: `thumbprint=${clientThumbprint}`,
        expectIngressKeyUsed: SERVER,
        expectEgressKeyUsed: CLIENT,
    },
    {
        keyFile: TRUSTED_SERVER_CERT,
        clusterSelector: `thumbprint=${clientThumbprint}`,
        expectIngressKeyUsed: SERVER,
        expectEgressKeyUsed: CLIENT,
    },
    {
        selector: `thumbprint=${serverThumbprint}`,
        clusterFile: TRUSTED_CLIENT_CERT,
        expectIngressKeyUsed: SERVER,
        expectEgressKeyUsed: CLIENT,
    },
    {
        selector: 'subject=Trusted Kernel Test Server',
        clusterSelector: 'subject=Trusted Kernel Test Client',
        expectIngressKeyUsed: SERVER,
        expectEgressKeyUsed: CLIENT,
    },
];

function testServerSelectorKeyUsage(testCase) {
    jsTestLog(`Running testServerSelectorKeyUsage with test case: ${tojson(testCase)}`);

    // Start a replica set with one mongod configured with the test case key file parameters
    // and a system CA store containing trusted-ca.pem.
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet({
        tlsMode: 'requireTLS',
        tlsCertificateKeyFile: testCase.keyFile,
        tlsCertificateSelector: testCase.selector,
        tlsClusterFile: testCase.clusterFile,
        tlsClusterCertificateSelector: testCase.clusterSelector,
        tlsAllowInvalidHostnames: "",
        tlsAllowConnectionsWithoutCertificates: "",
        waitForConnect: true,
        setParameter: {tlsUseSystemCA: true},
    });
    rst.initiate();
    rst.awaitReplication();
    let conn = rst.getPrimary();

    jsTestLog("Testing server uses correct key on ingress");
    assert.soon(function() {
        return runMongoProgram('mongo',
                               '--tls',
                               '--tlsAllowInvalidHostnames',
                               '--tlsCAFile',
                               TRUSTED_CA_CERT,
                               '--tlsCertificateKeyFile',
                               TRUSTED_CLIENT_CERT,
                               '--port',
                               conn.port,
                               '--eval',
                               'quit()') === 0;
    }, "mongo did not initialize properly");

    assert.soon(
        () => {
            const log = rawMongoProgramOutput(".*");
            return log.search(testCase.expectIngressKeyUsed) !== -1;
        },
        `logfile did not contain expected peer certificate info: ${
            testCase.expectIngressKeyUsed}.\n` +
            "Log File Contents\n==============================\n" + rawMongoProgramOutput(".*") +
            "\n==============================\n");

    jsTestLog("Testing server uses correct key on egress");

    // Add new node to test the other node's egress key
    let otherNode = rst.add({
        tlsMode: 'requireTLS',
        tlsCertificateKeyFile: TRUSTED_SERVER_CERT,
        tlsCAFile: TRUSTED_CA_CERT,
        tlsAllowInvalidHostnames: "",
        setParameter: {tlsWithholdClientCertificate: true},
        waitForConnect: true,
    });

    jsTestLog("Reinitiating replica set with one additional node");
    rst.reInitiate();
    rst.awaitSecondaryNodes();

    assert.commandWorked(otherNode.adminCommand({clearLog: 'global'}));

    // Verify node 1 can now connect to node 2
    jsTestLog("Forcing egress connection with replSetTestEgress...");
    assert.commandWorked(conn.adminCommand({replSetTestEgress: 1}));

    checkLog.containsRelaxedJson(
        otherNode, 6723802, {peerSubjectName: testCase.expectEgressKeyUsed});

    jsTestLog("Stopping the replica set...");
    rst.stopSet();
}

requireSSLProvider('windows', function() {
    if (_isWindows()) {
        assert.eq(0,
                  runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"));

        // SChannel backed follows Windows rules and only trusts Root in LocalMachine
        runProgram("certutil.exe", "-addstore", "-f", "Root", TRUSTED_CA_CERT);
        // Import a pfx file since it contains both a cert and private key and is easy to import
        // via command line.
        const importPfx = function(pfxFile) {
            return runProgram("certutil.exe", "-importpfx", "-f", "-p", "qwerty", pfxFile);
        };
        assert.eq(0, importPfx("jstests\\libs\\trusted-client.pfx"));
        assert.eq(0, importPfx("jstests\\libs\\trusted-server.pfx"));
    }

    try {
        testCases.forEach(test => testServerSelectorKeyUsage(test));
    } finally {
        if (_isWindows()) {
            const trusted_ca_thumbprint = cat('jstests/libs/trusted-ca.pem.digest.sha1');
            runProgram("certutil.exe", "-delstore", "-f", "Root", trusted_ca_thumbprint);
        }
    }
});
