// Test that a client can authenicate against the server with roles.
// Also validates RFC2253
load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

    const SERVER_CERT = "jstests/libs/server.pem";
    const CA_CERT = "jstests/libs/ca.pem";
    const CLIENT_CERT = "jstests/libs/client_roles.pem";
    const CLIENT_ESCAPE_CERT = "jstests/libs/client_escape.pem";
    const CLIENT_UTF8_CERT = "jstests/libs/client_utf8.pem";
    const CLIENT_EMAIL_CERT = "jstests/libs/client_email.pem";
    const CLIENT_TITLE_CERT = "jstests/libs/client_title.pem";
    const CLIENT_CERT_NO_ROLES = "jstests/libs/client.pem";

    const CLIENT_USER =
        "C=US,ST=New York,L=New York City,O=MongoDB,OU=Kernel Users,CN=Kernel Client Peer Role";

    const CLIENT_USER_NO_ROLES =
        "C=US,ST=New York,L=New York City,O=MongoDB,OU=KernelUser,CN=client";
    const smokeScript =
        'assert(db.getSiblingDB(\'$external\').auth({ mechanism: \'MONGODB-X509\' }));';

    function authAndTest(port, expectSuccess) {
        // First we run the shell with the "smoke" user that has no embedded roles to verify
        // that X509 auth works overall.
        const smoke = runMongoProgram("mongo",
                                      "--host",
                                      "localhost",
                                      "--port",
                                      port,
                                      "--ssl",
                                      "--sslCAFile",
                                      CA_CERT,
                                      "--sslPEMKeyFile",
                                      CLIENT_CERT_NO_ROLES,
                                      "--eval",
                                      smokeScript);
        assert.eq(smoke, 0, "Could not auth with smoke user");

        const runTest = function(cert, script) {
            const res = runMongoProgram("mongo",
                                        "--host",
                                        "localhost",
                                        "--port",
                                        port,
                                        "--ssl",
                                        "--sslCAFile",
                                        CA_CERT,
                                        "--sslPEMKeyFile",
                                        cert,
                                        script);

            let expectExitCode = 0;
            if (!expectSuccess) {
                if (_isWindows()) {
                    expectExitCode = -3;
                } else {
                    expectExitCode = 253;
                }
            }

            assert.eq(expectExitCode, res, "Connection attempt failed");
        };

        // Then we assert success or failure with each of the X509 certs with embedded roles.
        runTest(CLIENT_CERT, "jstests/ssl/libs/ssl_x509_role_auth.js");
        runTest(CLIENT_ESCAPE_CERT, "jstests/ssl/libs/ssl_x509_role_auth_escape.js");
        runTest(CLIENT_UTF8_CERT, "jstests/ssl/libs/ssl_x509_role_auth_utf8.js");
        runTest(CLIENT_EMAIL_CERT, "jstests/ssl/libs/ssl_x509_role_auth_email.js");
    }

    const prepConn = function(conn) {
        const admin = conn.getDB('admin');
        admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
        assert(admin.auth('admin', 'admin'));

        const external = conn.getDB('$external');
        external.createUser(
            {user: CLIENT_USER_NO_ROLES, roles: [{'role': 'readWrite', 'db': 'test'}]});
    };

    const x509_options = {sslMode: "requireSSL", sslPEMKeyFile: SERVER_CERT, sslCAFile: CA_CERT};

    print("1. Testing x.509 auth to mongod");
    {
        let mongo = MongoRunner.runMongod(Object.merge(x509_options, {auth: ""}));
        prepConn(mongo);

        authAndTest(mongo.port, true);

        MongoRunner.stopMongod(mongo);
    }

    jsTestLog("2. Testing disabling x.509 auth with roles");
    {
        const mongo = MongoRunner.runMongod(Object.merge(
            x509_options, {auth: "", setParameter: "allowRolesFromX509Certificates=false"}));

        prepConn(mongo);

        authAndTest(mongo.port, false);

        MongoRunner.stopMongod(mongo);
    }

    print("3. Testing x.509 auth to mongos");
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

        prepConn(st.s0);
        authAndTest(st.s0.port, true);
        st.stop();
    }

    print("4. Testing x.509 auth to mongos with x509 roles disabled");
    {
        const localOptions =
            Object.merge(x509_options, {setParameter: "allowRolesFromX509Certificates=false"});
        // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
        let st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                keyFile: 'jstests/libs/key1',
                configOptions: localOptions,
                mongosOptions: localOptions,
                shardOptions: localOptions,
                useHostname: false,
                shardAsReplicaSet: false
            }
        });

        prepConn(st.s0);
        authAndTest(st.s0.port, false);
        st.stop();
    }
}());
