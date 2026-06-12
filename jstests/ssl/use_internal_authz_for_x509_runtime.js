/**
 * Tests that the useInternalAuthzForX509 server parameter can be set at runtime via setParameter
 * and that X.509 authentication continues to work correctly after runtime changes to the parameter
 * on both standalone mongod and sharded cluster (mongos).
 *
 * Without LDAP configured, internal authorization is always used regardless of the parameter
 * value, so these tests verify:
 * 1. The parameter default value is false.
 * 2. The parameter can be changed to true at runtime.
 * 3. The parameter can be changed back to false at runtime.
 * 4. X.509 authentication succeeds in all cases.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const SERVER_CERT = getX509Path("server.pem");
const CA_CERT = getX509Path("ca.pem");
const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

const x509Options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    auth: "",
};

const x509ClusterOptions = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
};

// Sets up the admin user and creates the X.509 user via the localhost exception.
function setupUsers(conn) {
    const adminDB = conn.getDB("admin");
    adminDB.createUser({user: "admin", pwd: "admin", roles: ["root"]});
    assert(adminDB.auth("admin", "admin"));
    conn.getDB("$external").createUser({
        user: CLIENT_USER,
        roles: [{role: "readWriteAnyDatabase", db: "admin"}],
    });
    adminDB.logout();
    return adminDB;
}

// Authenticates as admin, runs the given callback, then logs out.
function withAdmin(adminDB, fn) {
    assert(adminDB.auth("admin", "admin"));
    try {
        fn(adminDB);
    } finally {
        adminDB.logout();
    }
}

// Verifies X.509 authentication against the given port succeeds.
// Opens a fresh connection each time: a connection authenticated once (e.g. as admin) cannot
// authenticate again, so X.509 checks must use their own unauthenticated connection.
function assertX509AuthSucceeds(host) {
    const conn = new Mongo(host);
    const ext = conn.getDB("$external");
    assert(ext.auth({mechanism: "MONGODB-X509"}), "X.509 authentication should succeed");
    ext.logout();
}

function runRuntimeParameterChecks(conn, label) {
    const adminDB = setupUsers(conn);

    withAdmin(adminDB, (db) => {
        const defaultResult = assert.commandWorked(
            db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}),
        );
        assert.eq(false, defaultResult.useInternalAuthzForX509, "default should be false");

        jsTest.log.info(
            "Testing X.509 auth with useInternalAuthzForX509=false (default) on " + label,
        );
        assertX509AuthSucceeds(conn.host);

        assert.commandWorked(db.runCommand({setParameter: 1, useInternalAuthzForX509: true}));

        const trueResult = assert.commandWorked(
            db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}),
        );
        assert.eq(true, trueResult.useInternalAuthzForX509, "should be true after setParameter");

        jsTest.log.info(
            "Testing X.509 auth with useInternalAuthzForX509=true (runtime change) on " + label,
        );
        assertX509AuthSucceeds(conn.host);

        assert.commandWorked(db.runCommand({setParameter: 1, useInternalAuthzForX509: false}));

        const restoredResult = assert.commandWorked(
            db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}),
        );
        assert.eq(false, restoredResult.useInternalAuthzForX509, "should be false after restoring");

        jsTest.log.info(
            "Testing X.509 auth after restoring useInternalAuthzForX509=false on " + label,
        );
        assertX509AuthSucceeds(conn.host);
    });
}

function runStartupParameterChecks(conn, label) {
    const adminDB = setupUsers(conn);

    withAdmin(adminDB, (db) => {
        const startupResult = assert.commandWorked(
            db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}),
        );
        assert.eq(
            true,
            startupResult.useInternalAuthzForX509,
            "should be true when set at startup",
        );

        jsTest.log.info(
            "Testing X.509 auth with useInternalAuthzForX509=true (set at startup) on " + label,
        );
        assertX509AuthSucceeds(conn.host);
    });
}

jsTest.log.info("Testing useInternalAuthzForX509 runtime updates on standalone mongod");
const mongod = MongoRunner.runMongod(x509Options);
runRuntimeParameterChecks(mongod, "standalone mongod");
MongoRunner.stopMongod(mongod);

// Verify that the parameter can also be set at startup and is reflected correctly.
jsTest.log.info("Testing useInternalAuthzForX509=true set at startup on standalone mongod");
const mongodWithParam = MongoRunner.runMongod(
    Object.merge(x509Options, {setParameter: {useInternalAuthzForX509: true}}),
);
runStartupParameterChecks(mongodWithParam, "standalone mongod");
MongoRunner.stopMongod(mongodWithParam);

jsTest.log.info("Testing useInternalAuthzForX509 runtime updates on sharded cluster");
{
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            keyFile: "jstests/libs/key1",
            configOptions: x509ClusterOptions,
            mongosOptions: x509ClusterOptions,
            rsOptions: x509ClusterOptions,
            useHostname: false,
        },
    });

    runRuntimeParameterChecks(st.s0, "sharded cluster");
    st.stop();
}

jsTest.log.info("Testing useInternalAuthzForX509=true set at startup on sharded cluster");
{
    const startupClusterOptions = Object.extend({}, x509ClusterOptions, true);
    startupClusterOptions.setParameter = {useInternalAuthzForX509: true};

    const stWithParam = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            keyFile: "jstests/libs/key1",
            configOptions: startupClusterOptions,
            mongosOptions: startupClusterOptions,
            rsOptions: startupClusterOptions,
            useHostname: false,
        },
    });

    runStartupParameterChecks(stWithParam.s0, "sharded cluster");
    stWithParam.stop();
}
