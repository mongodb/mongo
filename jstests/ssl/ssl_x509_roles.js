// Test that a client can authenicate against the server with roles.
// Also validates RFC2253
import {ShardingTest} from "jstests/libs/shardingtest.js";

const isWindows = _isWindows();
let ProxyProtocolServer;
if (!isWindows) {
    ({ProxyProtocolServer} = await import("jstests/sharding/libs/proxy_protocol.js"));
}

const SERVER_CERT = getX509Path("server.pem");
const CA_CERT = getX509Path("ca.pem");
const CLIENT_CERT = getX509Path("client_roles.pem");
const CLIENT_ESCAPE_CERT = getX509Path("client_escape.pem");
const CLIENT_UTF8_CERT = getX509Path("client_utf8.pem");
const CLIENT_EMAIL_CERT = getX509Path("client_email.pem");
const CLIENT_CERT_NO_ROLES = getX509Path("client.pem");
const CLIENT_USER_NO_ROLES = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";
const smokeScript = "assert(db.getSiblingDB('$external').auth({ mechanism: 'MONGODB-X509' }));";

function authAndTest(port, expectSuccess) {
    // First we run the shell with the "smoke" user that has no embedded roles to verify
    // that X509 auth works overall.
    const smoke = runMongoProgram(
        "mongo",
        "--host",
        "localhost",
        "--port",
        port,
        "--tls",
        "--tlsCAFile",
        CA_CERT,
        "--tlsCertificateKeyFile",
        CLIENT_CERT_NO_ROLES,
        "--eval",
        smokeScript,
    );
    assert.eq(smoke, 0, "Could not auth with smoke user");

    const runTest = function (cert, script) {
        const res = runMongoProgram(
            "mongo",
            "--host",
            "localhost",
            "--port",
            port,
            "--tls",
            "--tlsCAFile",
            CA_CERT,
            "--tlsCertificateKeyFile",
            cert,
            script,
        );

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

function isConnAuthenticated(conn) {
    const connStatus = conn.adminCommand({connectionStatus: 1, showPrivileges: true});
    const connIsAuthenticated = connStatus.authInfo.authenticatedUsers.length > 0;
    return connIsAuthenticated;
}

const prepConn = function (conn) {
    if (!isConnAuthenticated(conn)) {
        const admin = conn.getDB("admin");
        admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
        assert(admin.auth("admin", "admin"));
    }

    const external = conn.getDB("$external");
    external.createUser({user: CLIENT_USER_NO_ROLES, roles: [{"role": "readWrite", "db": "test"}]});
};

const x509_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
};

jsTest.log.info("1. Testing x.509 auth to mongod");
{
    let mongo = MongoRunner.runMongod(Object.merge(x509_options, {auth: ""}));
    prepConn(mongo);

    authAndTest(mongo.port, true);

    MongoRunner.stopMongod(mongo);
}

jsTest.log.info("2. Testing disabling x.509 auth with roles");
{
    const mongo = MongoRunner.runMongod(
        Object.merge(x509_options, {auth: "", setParameter: {allowRolesFromX509Certificates: false}}),
    );

    prepConn(mongo);

    authAndTest(mongo.port, false);

    MongoRunner.stopMongod(mongo);
}

jsTest.log.info("3. Testing x.509 auth to mongos");
{
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

    prepConn(st.s0);
    authAndTest(st.s0.port, true);
    st.stop();
}

jsTest.log.info("4. Testing x.509 auth to mongos with x509 roles disabled");
{
    const localOptions = Object.merge(x509_options, {setParameter: {allowRolesFromX509Certificates: false}});
    let st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            keyFile: "jstests/libs/key1",
            configOptions: localOptions,
            mongosOptions: localOptions,
            rsOptions: localOptions,
            useHostname: false,
        },
    });

    prepConn(st.s0);
    authAndTest(st.s0.port, false);
    st.stop();
}

// ============================================================================
// Proxy protocol tests: same authAndTest exercised through a TLS-terminating proxy that
// forwards X.509 identity (DN + roles) via PROXY protocol v2 TLVs over a Unix domain socket.
// Architecture:  [shell] --TLS--> [proxy] --PP2+UDS--> [mongod/mongos]
// ============================================================================
if (!isWindows) {
    /**
     * Creates a proxy protocol server that terminates TLS on ingress, extracts the client cert DN
     * and mongoRoles extension from the TLS handshake, and forwards them as PP2 TLVs to the
     * target's proxy Unix domain socket.
     */
    function startProxy(ingressPort, targetPort, socketPrefix) {
        const udsPath = `${socketPrefix}/unix-mongodb-${targetPort}.sock`;
        assert(fileExists(udsPath), `Proxy UDS should exist at ${udsPath}`);

        const proxy = new ProxyProtocolServer(ingressPort, targetPort, 2, {
            egressUnixSocket: udsPath,
            ingressTLSCert: SERVER_CERT,
            ingressTLSCA: CA_CERT,
        });
        proxy.start();
        return proxy;
    }

    jsTest.log.info("5. Testing x.509 auth with roles to mongod through proxy protocol");
    {
        const kSocketPrefix = `${MongoRunner.dataDir}/proxy_roles_mongod_sockets`;
        mkdir(kSocketPrefix);

        const mongod = MongoRunner.runMongod(
            Object.merge(x509_options, {auth: "", proxyUnixSocketPrefix: kSocketPrefix}),
        );
        prepConn(mongod);

        const kProxyIngressPort = allocatePort();
        const proxy = startProxy(kProxyIngressPort, mongod.port, kSocketPrefix);

        authAndTest(kProxyIngressPort, true);

        proxy.stop();
        MongoRunner.stopMongod(mongod);
    }

    jsTest.log.info("6. Testing disabling x.509 auth with roles to mongod through proxy protocol");
    {
        const kSocketPrefix = `${MongoRunner.dataDir}/proxy_roles_disabled_mongod_sockets`;
        mkdir(kSocketPrefix);

        const mongod = MongoRunner.runMongod(
            Object.merge(x509_options, {
                auth: "",
                setParameter: {allowRolesFromX509Certificates: false},
                proxyUnixSocketPrefix: kSocketPrefix,
            }),
        );
        prepConn(mongod);

        const kProxyIngressPort = allocatePort();
        const proxy = startProxy(kProxyIngressPort, mongod.port, kSocketPrefix);

        authAndTest(kProxyIngressPort, false);

        proxy.stop();
        MongoRunner.stopMongod(mongod);
    }

    jsTest.log.info("7. Testing x.509 auth with roles to mongos through proxy protocol");
    {
        const kSocketPrefix = `${MongoRunner.dataDir}/proxy_roles_mongos_sockets`;
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

        prepConn(st.s0);

        const kProxyIngressPort = allocatePort();
        const proxy = startProxy(kProxyIngressPort, st.s0.port, kSocketPrefix);

        authAndTest(kProxyIngressPort, true);

        proxy.stop();
        st.stop();
    }

    jsTest.log.info("8. Testing x.509 auth with roles disabled to mongos through proxy protocol");
    {
        const kSocketPrefix = `${MongoRunner.dataDir}/proxy_roles_disabled_mongos_sockets`;
        mkdir(kSocketPrefix);

        const localOptions = Object.merge(x509_options, {setParameter: {allowRolesFromX509Certificates: false}});
        let st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                keyFile: "jstests/libs/key1",
                configOptions: localOptions,
                mongosOptions: Object.merge(localOptions, {proxyUnixSocketPrefix: kSocketPrefix}),
                rsOptions: localOptions,
                useHostname: false,
            },
        });

        prepConn(st.s0);

        const kProxyIngressPort = allocatePort();
        const proxy = startProxy(kProxyIngressPort, st.s0.port, kSocketPrefix);

        authAndTest(kProxyIngressPort, false);

        proxy.stop();
        st.stop();
    }
}
