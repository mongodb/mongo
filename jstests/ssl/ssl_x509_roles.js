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

    const CLIENT_USER =
        "C=US,ST=New York,L=New York City,O=MerizoDB,OU=Kernel Users,CN=Kernel Client Peer Role";

    function authAndTest(port) {
        const merizo = runMerizoProgram("merizo",
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

        // runMerizoProgram returns 0 on success
        assert.eq(0, merizo, "Connection attempt failed");

        const escaped = runMerizoProgram("merizo",
                                        "--host",
                                        "localhost",
                                        "--port",
                                        port,
                                        "--ssl",
                                        "--sslCAFile",
                                        CA_CERT,
                                        "--sslPEMKeyFile",
                                        CLIENT_ESCAPE_CERT,
                                        "jstests/ssl/libs/ssl_x509_role_auth_escape.js");

        // runMerizoProgram returns 0 on success
        assert.eq(0, escaped, "Connection attempt failed");

        const utf8 = runMerizoProgram("merizo",
                                     "--host",
                                     "localhost",
                                     "--port",
                                     port,
                                     "--ssl",
                                     "--sslCAFile",
                                     CA_CERT,
                                     "--sslPEMKeyFile",
                                     CLIENT_UTF8_CERT,
                                     "jstests/ssl/libs/ssl_x509_role_auth_utf8.js");

        // runMerizoProgram returns 0 on success
        assert.eq(0, utf8, "Connection attempt failed");

        const email = runMerizoProgram("merizo",
                                      "--host",
                                      "localhost",
                                      "--port",
                                      port,
                                      "--ssl",
                                      "--sslCAFile",
                                      CA_CERT,
                                      "--sslPEMKeyFile",
                                      CLIENT_EMAIL_CERT,
                                      "jstests/ssl/libs/ssl_x509_role_auth_email.js");

        // runMerizoProgram returns 0 on success
        assert.eq(0, email, "Connection attempt failed");
    }

    const x509_options = {sslMode: "requireSSL", sslPEMKeyFile: SERVER_CERT, sslCAFile: CA_CERT};

    print("1. Testing x.509 auth to merizod");
    {
        let merizo = MerizoRunner.runMerizod(Object.merge(x509_options, {auth: ""}));

        authAndTest(merizo.port);

        MerizoRunner.stopMerizod(merizo);
    }

    print("2. Testing x.509 auth to merizos");
    {
        // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
        let st = new ShardingTest({
            shards: 1,
            merizos: 1,
            other: {
                keyFile: 'jstests/libs/key1',
                configOptions: x509_options,
                merizosOptions: x509_options,
                shardOptions: x509_options,
                useHostname: false,
                shardAsReplicaSet: false
            }
        });

        authAndTest(st.s0.port);
        st.stop();
    }
}());
