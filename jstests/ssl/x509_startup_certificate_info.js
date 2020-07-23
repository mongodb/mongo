// Test for logging of certificate information
(function() {
'use strict';

load("jstests/ssl/libs/ssl_helpers.js");

const CA_CERT = "jstests/libs/ca.pem";
const SERVER_CERT = "jstests/libs/server.pem";
const CLUSTER_CERT = "jstests/libs/cluster_cert.pem";
const CRL_FILE = "jstests/libs/crl.pem";

const SERVER_CERT_INFO = {
    "type": "Server",
    "subject": "CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US",
    "issuer": "CN=Kernel Test CA,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US",
    "thumbprint": "BF2E341D28D7CEAADA534A11D75189D4ECABB551"
};
const CLUSTER_CERT_INFO = {
    "type": "Cluster",
    "subject": "CN=clustertest,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US",
    "issuer": "CN=Kernel Test CA,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US",
    "thumbprint": "FD85F9F6F380EE53F46F497253453731DC885335"
};
const CRL_INFO = {
    "thumbprint": "551FEF8D916CE363E5488AD7F4BD60E3D1EC2BD8"
};

function runTest(checkMongos,
                 opts,
                 expectServerInfo,
                 expectClusterInfo,
                 expectCRLInfo,
                 serverInfoToExpect,
                 clusterInfoToExpect,
                 CRLInfotoExpect) {
    let mongo;

    if (checkMongos) {
        var st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other:
                {configOptions: opts, mongosOptions: opts, shardOptions: opts, useHostname: false}
        });
        mongo = st.s;
    } else {
        mongo = MongoRunner.runMongod(Object.assign(opts));
    }

    assert.soon(function() {
        return (expectServerInfo ===
                checkLog.checkContainsOnceJson(mongo, 4913010, serverInfoToExpect));
    });

    if (!(determineSSLProvider() === "windows" && !expectClusterInfo)) {
        assert.soon(function() {
            return (expectClusterInfo ===
                    checkLog.checkContainsOnceJson(mongo, 4913011, clusterInfoToExpect));
        });
    }

    if (!(determineSSLProvider() === "apple")) {
        assert.soon(function() {
            return (expectCRLInfo ===
                    checkLog.checkContainsOnceJson(mongo, 4913012, CRLInfotoExpect));
        });
    }

    if (checkMongos) {
        st.stop();
    } else {
        stopMongoProgramByPid(mongo.pid);
    }
}

function runTests(checkMongos) {
    runTest(checkMongos,
            {
                sslMode: 'requireSSL',
                tlsCertificateKeyFile: SERVER_CERT,
                tlsCAFile: CA_CERT,
                tlsClusterFile: CLUSTER_CERT,
                tlsCRLFile: CRL_FILE,
                useHostname: false
            },
            true,
            true,
            true,
            SERVER_CERT_INFO,
            CLUSTER_CERT_INFO,
            CRL_INFO);

    runTest(checkMongos,
            {
                sslMode: 'requireSSL',
                tlsCertificateKeyFile: SERVER_CERT,
                tlsCAFile: CA_CERT,
                tlsClusterFile: CLUSTER_CERT,
            },
            true,
            true,
            false,
            SERVER_CERT_INFO,
            CLUSTER_CERT_INFO,
            {});

    runTest(checkMongos,
            {
                sslMode: 'requireSSL',
                tlsCertificateKeyFile: SERVER_CERT,
                sslCAFile: CA_CERT,
                sslCRLFile: CRL_FILE,
            },
            true,
            false,
            true,
            SERVER_CERT_INFO,
            CLUSTER_CERT_INFO,
            CRL_INFO);
}

// runTests(true);
runTests(false);
})();
