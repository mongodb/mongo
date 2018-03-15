// Test that a client can authenicate against the server with roles.
(function() {
    "use strict";

    const SERVER_CERT = "jstests/libs/server.pem";
    const CA_CERT = "jstests/libs/ca.pem";
    const CLIENT_CERT = "jstests/libs/client_roles.pem";

    const CLIENT_USER =
        "C=US,ST=New York,L=New York City,O=MongoDB,OU=Kernel Users,CN=Kernel Client Peer Role";

    function authAndTest(port) {
        let mongo = runMongoProgram("mongo",
                                    "--host",
                                    "localhost",
                                    "--port",
                                    port,
                                    "--ssl",
                                    "--sslCAFile",
                                    CA_CERT,
                                    "--sslPEMKeyFile",
                                    CLIENT_CERT,
                                    "jstests/ssl/libs/ssl_x509_role_auth.js");

        // runMongoProgram returns 0 on success
        assert.eq(0, mongo, "Connection attempt failed");
    }

    let x509_options = {sslMode: "requireSSL", sslPEMKeyFile: SERVER_CERT, sslCAFile: CA_CERT};

    print("1. Testing x.509 auth to mongod");
    {
        let mongo = MongoRunner.runMongod(Object.merge(x509_options, {auth: ""}));

        authAndTest(mongo.port);

        MongoRunner.stopMongod(mongo);
    }

    print("2. Testing x.509 auth to mongos");
    {
        // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
        let st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                keyFile: 'jstests/libs/key1',
                configOptions: x509_options,
                mongosOptions: x509_options,
                shardOptions: x509_options,
                useHostname: false,
                shardAsReplicaSet: false
            }
        });

        authAndTest(st.s0.port);
        st.stop();
    }
}());
