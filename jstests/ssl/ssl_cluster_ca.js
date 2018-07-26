// Verify certificates and CAs between intra-cluster
// and client->server communication using different CAs.

(function() {
    "use strict";

    function testRS(opts, succeed) {
        const origSkipCheck = TestData.skipCheckDBHashes;
        const rsOpts = {
            // Use localhost so that SAN matches.
            useHostName: false,
            nodes: {node0: opts, node1: opts},
        };
        const rs = new ReplSetTest(rsOpts);
        rs.startSet();
        if (succeed) {
            rs.initiate();
            assert.commandWorked(rs.getPrimary().getDB('admin').runCommand({isMaster: 1}));
        } else {
            assert.throws(function() {
                rs.initiate();
            });
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
        sslMode: 'requireSSL',
        // Servers present trusted-server.pem to clients and each other for inbound connections.
        // Peers validate trusted-server.pem using trusted-ca.pem when making those connections.
        sslPEMKeyFile: 'jstests/libs/trusted-server.pem',
        sslCAFile: 'jstests/libs/trusted-ca.pem',
        // Servers making outbound connections to other servers present server.pem to their peers
        // which their peers validate using ca.pem.
        sslClusterFile: 'jstests/libs/server.pem',
        sslClusterCAFile: 'jstests/libs/ca.pem',
        // SERVER-36895: IP based hostname validation with SubjectAlternateName
        sslAllowInvalidHostnames: '',
    };

    testRS(valid_options, true);

    const wrong_cluster_file =
        Object.assign({}, valid_options, {sslClusterFile: valid_options.sslPEMKeyFile});
    testRS(wrong_cluster_file, false);

    const wrong_key_file =
        Object.assign({}, valid_options, {sslPEMKeyFile: valid_options.sslClusterFile});
    testRS(wrong_key_file, false);

    const mongod = MongoRunner.runMongod(valid_options);
    assert(mongod, "Failed starting standalone mongod with alternate CA");

    function testConnect(cert, succeed) {
        const mongo = runMongoProgram("mongo",
                                      "--host",
                                      "localhost",
                                      "--port",
                                      mongod.port,
                                      "--ssl",
                                      "--sslCAFile",
                                      valid_options.sslCAFile,
                                      "--sslPEMKeyFile",
                                      cert,
                                      "--eval",
                                      ";");

        // runMongoProgram returns 0 on success
        assert.eq(mongo === 0, succeed);
    }

    testConnect('jstests/libs/client.pem', true);
    testConnect('jstests/libs/trusted-client.pem', false);

    MongoRunner.stopMongod(mongod);
}());
