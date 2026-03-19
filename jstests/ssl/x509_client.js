// Check if this build supports the authenticationMechanisms startup parameter.

import {ShardingTest} from "jstests/libs/shardingtest.js";

const isWindows = _isWindows();
let ProxyProtocolServer;
if (!isWindows) {
    ({ProxyProtocolServer} = await import("jstests/sharding/libs/proxy_protocol.js"));
}

const SERVER_CERT = getX509Path("server.pem");
const CA_CERT = getX509Path("ca.pem");
const CLIENT_CERT = getX509Path("client.pem");

const INTERNAL_USER = "CN=internal,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US";
const SERVER_USER = "CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US";
const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";
const INVALID_CLIENT_USER = "C=US,ST=New York,L=New York City,O=MongoDB,OU=KernelUser,CN=invalid";

function authAndTest(mongo) {
    let external = mongo.getDB("$external");
    let test = mongo.getDB("test");

    // Add user using localhost exception
    external.createUser({
        user: CLIENT_USER,
        roles: [
            {"role": "userAdminAnyDatabase", "db": "admin"},
            {"role": "readWriteAnyDatabase", "db": "admin"},
            {"role": "clusterMonitor", "db": "admin"},
        ],
    });

    // Localhost exception should not be in place anymore
    assert.throws(
        function () {
            test.foo.findOne();
        },
        [],
        "read without login",
    );

    assert(
        !external.auth({user: INVALID_CLIENT_USER, mechanism: "MONGODB-X509"}),
        "authentication with invalid user should fail",
    );
    assert(external.auth({user: CLIENT_USER, mechanism: "MONGODB-X509"}), "authentication with valid user failed");
    assert(
        external.auth({mechanism: "MONGODB-X509"}),
        "authentication with valid client cert and no user field failed",
    );

    const withUserReply = assert.commandWorked(
        external.runCommand({authenticate: 1, mechanism: "MONGODB-X509", user: CLIENT_USER}),
        "runCommand authentication with valid client cert and user field failed",
    );
    assert.eq(withUserReply.user, CLIENT_USER);
    assert.eq(withUserReply.dbname, "$external");

    const noUserReply = assert.commandWorked(
        external.runCommand({authenticate: 1, mechanism: "MONGODB-X509"}),
        "runCommand authentication with valid client cert and no user field failed",
    );
    assert.eq(noUserReply.user, CLIENT_USER);
    assert.eq(noUserReply.dbname, "$external");

    // Check that there's a "Successfully authenticated" message that includes the client
    // address. For direct TLS connections this is an IP:port (e.g. "127.0.0.1:35098"); for
    // proxy protocol connections over a Unix domain socket it is "anonymous unix socket:<port>".
    const log = assert.commandWorked(external.getSiblingDB("admin").runCommand({getLog: "global"})).log;

    function checkAuthSuccess(element /*, index, array*/) {
        const logJson = JSON.parse(element);

        return (
            logJson.id === 5286306 &&
            logJson.attr.user === CLIENT_USER &&
            logJson.attr.db === "$external" &&
            /(?:(?:\d{1,3}\.){3}\d{1,3}:\d+|anonymous unix socket:\d+)/.test(logJson.attr.client)
        );
    }
    assert(log.some(checkAuthSuccess));

    // It should be impossible to create users with the same name as the server's subject,
    // unless guardrails are explicitly overridden
    assert.commandFailedWithCode(
        external.runCommand({createUser: SERVER_USER, roles: [{"role": "userAdminAnyDatabase", "db": "admin"}]}),
        ErrorCodes.BadValue,
        "Created user with same name as the server's x.509 subject",
    );

    // It should be impossible to create users with names recognized as cluster members,
    // unless guardrails are explicitly overridden
    assert.commandFailedWithCode(
        external.runCommand({createUser: INTERNAL_USER, roles: [{"role": "userAdminAnyDatabase", "db": "admin"}]}),
        ErrorCodes.BadValue,
        "Created user which would be recognized as a cluster member",
    );

    // Check that we can add a user and read data
    test.createUser({user: "test", pwd: "test", roles: [{"role": "readWriteAnyDatabase", "db": "admin"}]});
    test.foo.findOne();

    external.logout();
    assert.throws(
        function () {
            test.foo.findOne();
        },
        [],
        "read after logout",
    );
}

const x509_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
};

{
    print("1. Testing x.509 auth to mongod");
    const mongo = MongoRunner.runMongod(Object.merge(x509_options, {auth: ""}));

    authAndTest(mongo);
    MongoRunner.stopMongod(mongo);
}

{
    print("2. Testing x.509 auth to mongos");
    let st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            keyFile: "jstests/libs/key1",
            configOptions: x509_options,
            mongosOptions: x509_options,
            rsOptions: x509_options,
            useHostname: false,
        },
    });

    authAndTest(new Mongo("localhost:" + st.s0.port));
    st.stop();
}

// ============================================================================
// Proxy protocol tests: same authAndTest exercised through a TLS-terminating proxy that
// forwards X.509 identity via PROXY protocol v2 TLVs over a Unix domain socket.
// Architecture:  [shell] --TLS--> [proxy] --PP2+UDS--> [mongod/mongos]
// ============================================================================
if (!isWindows && !jsTestOptions().shellGRPC) {
    /**
     * Creates a proxy protocol server that terminates TLS on ingress, extracts the client cert DN
     * (and roles if present) from the TLS handshake, and forwards them as PP2 TLVs to the target's
     * proxy Unix domain socket.
     *
     * @param {number} ingressPort - Port the proxy listens on for client TLS connections.
     * @param {number} targetPort - The mongod/mongos port (used to derive the UDS path).
     * @param {string} socketPrefix - The proxyUnixSocketPrefix directory.
     * @returns {ProxyProtocolServer} A started proxy server instance.
     */
    function startProxy(ingressPort, targetPort, socketPrefix) {
        const udsPath = `${socketPrefix}/proxy-mongodb-${targetPort}.sock`;
        assert(fileExists(udsPath), `Proxy UDS should exist at ${udsPath}`);

        const proxy = new ProxyProtocolServer(ingressPort, targetPort, 2, {
            egressUnixSocket: udsPath,
            ingressTLSCert: SERVER_CERT,
            ingressTLSCA: CA_CERT,
        });
        proxy.start();
        return proxy;
    }

    /**
     * Creates a Mongo connection through the proxy. The shell connects over TLS with client.pem,
     * the proxy extracts the DN and forwards it via PP2 TLV 0xE0 to the server.
     */
    function connectViaProxy(ingressPort) {
        return new Mongo("localhost:" + ingressPort, undefined, {
            tls: {certificateKeyFile: CLIENT_CERT, CAFile: CA_CERT},
        });
    }

    {
        print("3. Testing x.509 auth to mongod through proxy protocol");
        const kSocketPrefix = `${MongoRunner.dataDir}/proxy_x509_mongod_sockets`;
        mkdir(kSocketPrefix);

        const mongod = MongoRunner.runMongod(
            Object.merge(x509_options, {auth: "", proxyUnixSocketPrefix: kSocketPrefix}),
        );

        const kProxyIngressPort = allocatePort();
        const proxy = startProxy(kProxyIngressPort, mongod.port, kSocketPrefix);

        authAndTest(connectViaProxy(kProxyIngressPort));

        proxy.stop();
        MongoRunner.stopMongod(mongod);
    }

    {
        print("4. Testing x.509 auth to mongos through proxy protocol");
        const kSocketPrefix = `${MongoRunner.dataDir}/proxy_x509_mongos_sockets`;
        mkdir(kSocketPrefix);

        let st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                keyFile: "jstests/libs/key1",
                configOptions: x509_options,
                mongosOptions: Object.merge(x509_options, {proxyUnixSocketPrefix: kSocketPrefix}),
                rsOptions: x509_options,
                useHostname: false,
            },
        });

        const kProxyIngressPort = allocatePort();
        const proxy = startProxy(kProxyIngressPort, st.s0.port, kSocketPrefix);

        authAndTest(connectViaProxy(kProxyIngressPort));

        proxy.stop();
        st.stop();
    }
}
