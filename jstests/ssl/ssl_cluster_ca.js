// Verify certificates and CAs between intra-cluster
// and client->server communication using different CAs.

import {ReplSetTest} from "jstests/libs/replsettest.js";

function testRS(node0_opts, node1_opts, succeed) {
    const origSkipCheck = TestData.skipCheckDBHashes;
    const rsOpts = {
        // Use localhost so that SAN matches.
        useHostName: false,
        nodes: {node0: node0_opts, node1: node1_opts},
    };
    const rs = new ReplSetTest(rsOpts);
    rs.startSet();
    if (succeed) {
        rs.initiate();
        assert.commandWorked(rs.getPrimary().getDB("admin").runCommand({hello: 1}));
    } else {
        // By default, rs.initiate takes a very long time to timeout. We should shorten this
        // period, because we expect it to fail. ReplSetTest has both a static and local copy
        // of kDefaultTimeOutMS, so we must override both.
        const oldTimeout = ReplSetTest.kDefaultTimeoutMS;
        const shortTimeout = 60 * 1000;
        ReplSetTest.kDefaultTimeoutMS = shortTimeout;
        rs.timeoutMS = shortTimeout;
        // The rs.initiate will fail in an assert.soon, which would ordinarily trigger the hang
        // analyzer.  We don't want that to happen, so we disable it here.
        MongoRunner.runHangAnalyzer.disable();
        try {
            assert.throws(function () {
                rs.initiate();
            });
        } finally {
            ReplSetTest.kDefaultTimeoutMS = oldTimeout;
            MongoRunner.runHangAnalyzer.enable();
        }
        TestData.skipCheckDBHashes = true;
    }
    rs.stopSet();
    TestData.skipCheckDBHashes = origSkipCheck;
}

// The name "trusted" in these certificates is misleading.
// They're just a separate trust chain from the ones without the name.
// ca.pem signed client.pem and server.pem
// trusted-ca.pem signed trusted-client.pem and trusted-server.pem
const valid_options = {
    tlsMode: "requireTLS",
    // Servers present trusted-server.pem to clients and each other for inbound connections.
    // Peers validate trusted-server.pem using trusted-ca.pem when making those connections.
    tlsCertificateKeyFile: "jstests/libs/trusted-server.pem",
    tlsCAFile: "jstests/libs/trusted-ca.pem",
    // Servers making outbound connections to other servers present server.pem to their peers
    // which their peers validate using ca.pem.
    tlsClusterFile: "jstests/libs/server.pem",
    tlsClusterCAFile: "jstests/libs/ca.pem",
    // SERVER-36895: IP based hostname validation with SubjectAlternateName
    tlsAllowInvalidHostnames: "",
};

testRS(valid_options, valid_options, true);

const wrong_cluster_file = Object.assign({}, valid_options, {tlsClusterFile: valid_options.tlsCertificateKeyFile});
testRS(wrong_cluster_file, wrong_cluster_file, false);

const wrong_key_file = Object.assign({}, valid_options, {tlsCertificateKeyFile: valid_options.tlsClusterFile});
testRS(wrong_key_file, wrong_key_file, false);

// Test self-signed clusterFile validated against peer's CAFile
const cafile_only_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    tlsAllowInvalidHostnames: "",
    clusterAuthMode: "x509",
};
const selfsigned_cluster_file = Object.merge(cafile_only_options, {tlsClusterFile: "jstests/libs/smoke.pem"});
testRS(cafile_only_options, selfsigned_cluster_file, false);

const mongod = MongoRunner.runMongod(valid_options);
assert(mongod, "Failed starting standalone mongod with alternate CA");

function testConnect(cert, succeed) {
    const mongo = runMongoProgram(
        "mongo",
        "--host",
        "localhost",
        "--port",
        mongod.port,
        "--tls",
        "--tlsCAFile",
        valid_options.tlsCAFile,
        "--tlsCertificateKeyFile",
        cert,
        "--eval",
        ";",
    );

    // runMongoProgram returns 0 on success
    assert.eq(mongo === 0, succeed);
}

testConnect("jstests/libs/client.pem", true);
testConnect("jstests/libs/trusted-client.pem", false);

MongoRunner.stopMongod(mongod);
