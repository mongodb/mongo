/**
 * Tests the enableInternalAuthzFallback server parameter. On master this parameter is a no-op:
 * clients can set it, but it has no effect. The parameter exists on master only to allow
 * backported branches to include startup/runtime commands that reference it without erroring.
 *
 * This test verifies:
 * 1. The parameter default value is false on master.
 * 2. The parameter can be changed to true and back to false at runtime via setParameter.
 * 3. The parameter can be set at startup.
 * 4. X.509 authentication continues to work regardless of the parameter value, since the parameter
 *    has no effect on master (behavior is governed by the feature flag, not this parameter).
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

// Verifies X.509 authentication against the given host succeeds.
function assertX509AuthSucceeds(host) {
    const conn = new Mongo(host);
    const ext = conn.getDB("$external");
    assert(ext.auth({mechanism: "MONGODB-X509"}), "X.509 authentication should succeed");
    ext.logout();
}

function getParam(db) {
    return assert.commandWorked(db.runCommand({getParameter: 1, enableInternalAuthzFallback: 1}))
        .enableInternalAuthzFallback;
}

function runRuntimeParameterChecks(conn, label) {
    const adminDB = setupUsers(conn);

    withAdmin(adminDB, (db) => {
        assert.eq(false, getParam(db), "default should be false on master");

        jsTest.log.info(
            "Testing X.509 auth with enableInternalAuthzFallback=false (default, no-op on master) on " +
                label,
        );
        assertX509AuthSucceeds(conn.host);

        assert.commandWorked(db.runCommand({setParameter: 1, enableInternalAuthzFallback: true}));
        assert.eq(true, getParam(db), "should be true after setParameter");

        jsTest.log.info(
            "Testing X.509 auth with enableInternalAuthzFallback=true (no-op on master) on " +
                label,
        );
        assertX509AuthSucceeds(conn.host);

        assert.commandWorked(db.runCommand({setParameter: 1, enableInternalAuthzFallback: false}));
        assert.eq(false, getParam(db), "should be false after restoring");

        jsTest.log.info(
            "Testing X.509 auth after restoring enableInternalAuthzFallback=false on " + label,
        );
        assertX509AuthSucceeds(conn.host);
    });
}

function runStartupParameterChecks(conn, label) {
    const adminDB = setupUsers(conn);

    withAdmin(adminDB, (db) => {
        assert.eq(true, getParam(db), "should be true when set to true at startup");

        jsTest.log.info(
            "Testing X.509 auth with enableInternalAuthzFallback=true (set at startup, no-op on master) on " +
                label,
        );
        assertX509AuthSucceeds(conn.host);
    });
}

jsTest.log.info("Testing enableInternalAuthzFallback runtime updates on standalone mongod");
const mongod = MongoRunner.runMongod(x509Options);
runRuntimeParameterChecks(mongod, "standalone mongod");
MongoRunner.stopMongod(mongod);

// Verify that the parameter can also be set at startup and is reflected correctly.
jsTest.log.info("Testing enableInternalAuthzFallback=true set at startup on standalone mongod");
const mongodWithParam = MongoRunner.runMongod(
    Object.merge(x509Options, {setParameter: {enableInternalAuthzFallback: true}}),
);
runStartupParameterChecks(mongodWithParam, "standalone mongod");
MongoRunner.stopMongod(mongodWithParam);

jsTest.log.info("Testing enableInternalAuthzFallback runtime updates on sharded cluster");
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

jsTest.log.info("Testing enableInternalAuthzFallback=true set at startup on sharded cluster");
{
    const startupClusterOptions = Object.extend({}, x509ClusterOptions, true);
    startupClusterOptions.setParameter = {enableInternalAuthzFallback: true};

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
