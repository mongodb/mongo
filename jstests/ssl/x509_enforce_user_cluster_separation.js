// Check if this build supports the authenticationMechanisms startup parameter.

const SERVER_CERT = "jstests/libs/server.pem";
const SERVER_SAN_CERT = "jstests/libs/server_SAN.pem";
const CLIENT_CERT = "jstests/libs/client.pem";
const CA_CERT = "jstests/libs/ca.pem";

const SERVER_USER = "CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US";
const SERVER_SAN_USER =
    "CN=Kernel Client Peer Role,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US";
const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

function authAndTest(cert, user) {
    const INVALID_USER = "C=US,ST=New York,L=New York City,O=MongoDB,OU=KernelUser,CN=invalid";

    let external = db.getSiblingDB("$external");
    let test = db.getSiblingDB("test");

    assert(!external.auth({user: INVALID_USER, mechanism: 'MONGODB-X509'}),
           "authentication with invalid user should fail");
    assert(external.auth({user: user, mechanism: 'MONGODB-X509'}),
           "authentication with valid user failed");
    assert(external.auth({mechanism: 'MONGODB-X509'}),
           "authentication with valid cert and no user field failed");
    assert(external.runCommand({authenticate: 1, mechanism: 'MONGODB-X509', user: user}).ok,
           "runCommand authentication with valid cert and user field failed");
    assert(external.runCommand({authenticate: 1, mechanism: 'MONGODB-X509'}).ok,
           "runCommand authentication with valid cert and no user field failed");
    // Smoke our current user with a find.
    test.foo.findOne();

    // Check that we can add a user and read data.
    test.createUser(
        {user: "test", pwd: "test", roles: [{'role': 'readWriteAnyDatabase', 'db': 'admin'}]});
    test.foo.findOne();

    // Reads are not allowed after logout.
    external.logout();
    assert.throws(function() {
        test.foo.findOne();
    }, [], "read after logout");
}

function runSubShell(conn, cert, user, func) {
    const args = [
        'mongo',
        '--tls',
        `--tlsCAFile=${CA_CERT}`,
        `--tlsCertificateKeyFile=${cert}`,
        '--tlsAllowInvalidHostnames',
        '--authenticationDatabase=$external',
        '--authenticationMechanism=MONGODB-X509',
        `mongodb://${conn.host}`,
        '--eval',
        `(${func.toString()})('${cert}', '${user}');`
    ];
    const ret = _runMongoProgram(...args);
    assert(ret == ErrorCodes.OK, 'subshell did not succeed');
}

function initUser(conn, user) {
    const external = conn.getDB("$external");
    external.createUser({
        user: user,
        roles: [
            {'role': 'userAdminAnyDatabase', 'db': 'admin'},
            {'role': 'readWriteAnyDatabase', 'db': 'admin'},
            {'role': 'clusterMonitor', 'db': 'admin'},
        ]
    });

    // Localhost exception should not be in place anymore
    const test = conn.getDB("test");
    assert.throws(function() {
        test.foo.findOne();
    }, [], "read without login");
}

const x509_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT
};

const mongodOptions =
    Object.merge(x509_options, {auth: "", setParameter: {enforceUserClusterSeparation: false}});

const mongosOptions =
    Object.merge(x509_options, {setParameter: {enforceUserClusterSeparation: false}});

function runMongodTest(desc, func) {
    print(desc);
    const mongo = MongoRunner.runMongod(mongodOptions);
    func(mongo);

    MongoRunner.stopMongod(mongo);
}

function runMongodFailTest(desc, options) {
    print(desc);
    assert.throws(() => MongoRunner.runMongod(Object.merge(mongodOptions, options)),
                  [],
                  "MongoD started successfully with bad options");
}

function runMongosTest(desc, func) {
    print(desc);
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            keyFile: 'jstests/libs/key1',
            configOptions: mongodOptions,
            mongosOptions: mongosOptions,
            shardOptions: x509_options,
            useHostname: false
        }
    });

    const mongo = new Mongo(`localhost:${st.s0.port}`);
    func(mongo);
    st.stop();
}

function runMongosFailTest(desc, options) {
    print(desc);
    // We start the ShardingTest cleanly first because it throws and fails to clean up after itself.
    const st = new ShardingTest({
        config: 1,
        shards: 1,
        mongos: 1,
        other: {
            keyFile: 'jstests/libs/key1',
            configOptions: mongodOptions,
            mongosOptions: mongosOptions,
            shardOptions: x509_options,
            useHostname: false
        }
    });

    const failOptions = Object.merge(mongosOptions, options);
    print(`Fail options: ${tojson(failOptions)}`);

    assert.throws(function() {
        // Start a new mongos with bad options.
        st.restartMongos(0, failOptions);
    }, [], "MongoS restarted successfully with bad options");

    // Avoid st.stop() because it will throw when it attempts to stop the second mongos.
    st.stopAllShards();
    st.stopAllConfigServers();
}

runMongodTest("1a. Testing x.509 auth to mongod with a client user/cert", function(conn) {
    initUser(conn, CLIENT_USER);

    runSubShell(conn, CLIENT_CERT, CLIENT_USER, authAndTest);
});

runMongodTest("1b. Testing x.509 auth to mongod with the server user/cert", function(conn) {
    initUser(conn, SERVER_USER);

    runSubShell(conn, SERVER_CERT, SERVER_USER, authAndTest);
});

runMongodTest("1c. Testing x.509 auth to mongod with a cluster user/cert", function(conn) {
    initUser(conn, SERVER_SAN_USER);

    runSubShell(conn, SERVER_SAN_CERT, SERVER_SAN_USER, authAndTest);
});

runMongodFailTest('1d. Testing x.509 cluster auth on mongod with "x509" option',
                  {clusterAuthMode: "x509"});

runMongodFailTest('1e. Testing x.509 cluster auth on mongod with "sendX509" option',
                  {clusterAuthMode: "sendX509"});

runMongodFailTest('1e. Testing x.509 cluster auth on mongod with "sendKeyFile" option',
                  {clusterAuthMode: "sendKeyFile"});

runMongosTest("2a. Testing x.509 auth to mongos with a client user/cert", function(conn) {
    initUser(conn, CLIENT_USER);

    runSubShell(conn, CLIENT_CERT, CLIENT_USER, authAndTest);
});

runMongosTest("2b. Testing x.509 auth to mongos with the server user/cert", function(conn) {
    initUser(conn, SERVER_USER);

    runSubShell(conn, SERVER_CERT, SERVER_USER, authAndTest);
});

runMongosTest("2c. Testing x.509 auth to mongos with a cluster user/cert", function(conn) {
    initUser(conn, SERVER_SAN_USER);

    runSubShell(conn, SERVER_SAN_CERT, SERVER_SAN_USER, authAndTest);
});

runMongosFailTest('2d. Testing x.509 cluster auth on mongos with "x509" option',
                  {restart: true, clusterAuthMode: "x509"});

runMongosFailTest('2e. Testing x.509 cluster auth on mongos with "sendX509" option',
                  {clusterAuthMode: "sendX509"});

runMongosFailTest('2f. Testing x.509 cluster auth on mongos with "sendKeyFile" option',
                  {clusterAuthMode: "sendKeyFile"});
