/**
 * Tests that the useInternalAuthzForX509 server parameter can be set at runtime via setParameter
 * and that X.509 authentication continues to work correctly after runtime changes to the parameter.
 *
 * Without LDAP configured, internal authorization is always used regardless of the parameter
 * value, so these tests verify:
 * 1. The parameter default value is false.
 * 2. The parameter can be changed to true at runtime.
 * 3. The parameter can be changed back to false at runtime.
 * 4. X.509 authentication succeeds in all cases.
 */

const SERVER_CERT = getX509Path("server.pem");
const CA_CERT = getX509Path("ca.pem");
const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

const x509Options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    auth: "",
};

// Sets up the admin user and creates the X.509 user via the localhost exception.
function setupUsers(conn) {
    const adminDB = conn.getDB("admin");
    adminDB.createUser({user: "admin", pwd: "admin", roles: ["root"]});
    assert(adminDB.auth("admin", "admin"));
    conn.getDB("$external").createUser({user: CLIENT_USER, roles: [{role: "readWriteAnyDatabase", db: "admin"}]});
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
function assertX509AuthSucceeds(port) {
    const conn = new Mongo("localhost:" + port);
    const ext = conn.getDB("$external");
    assert(ext.auth({mechanism: "MONGODB-X509"}), "X.509 authentication should succeed");
    ext.logout();
}

jsTest.log.info("Testing useInternalAuthzForX509 can be read and changed at runtime");

const mongod = MongoRunner.runMongod(x509Options);
const adminDB = setupUsers(mongod);

withAdmin(adminDB, (db) => {
    // Verify the default value is false.
    const defaultResult = assert.commandWorked(db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}));
    assert.eq(false, defaultResult.useInternalAuthzForX509, "default should be false");

    // X.509 auth should succeed with useInternalAuthzForX509=false (default).
    jsTest.log.info("Testing X.509 auth with useInternalAuthzForX509=false (default)");
    assertX509AuthSucceeds(mongod.port);

    // Change the parameter to true at runtime.
    assert.commandWorked(db.runCommand({setParameter: 1, useInternalAuthzForX509: true}));

    const trueResult = assert.commandWorked(db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}));
    assert.eq(true, trueResult.useInternalAuthzForX509, "should be true after setParameter");

    // X.509 auth should still succeed with useInternalAuthzForX509=true. Without LDAP,
    // internal authorization is always used so behavior is unchanged.
    jsTest.log.info("Testing X.509 auth with useInternalAuthzForX509=true (runtime change)");
    assertX509AuthSucceeds(mongod.port);

    // Change the parameter back to false at runtime.
    assert.commandWorked(db.runCommand({setParameter: 1, useInternalAuthzForX509: false}));

    const restoredResult = assert.commandWorked(db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}));
    assert.eq(false, restoredResult.useInternalAuthzForX509, "should be false after restoring");

    // X.509 auth should still succeed after restoring the parameter.
    jsTest.log.info("Testing X.509 auth after restoring useInternalAuthzForX509=false");
    assertX509AuthSucceeds(mongod.port);
});

MongoRunner.stopMongod(mongod);

// Verify that the parameter can also be set at startup and is reflected correctly.
jsTest.log.info("Testing useInternalAuthzForX509=true set at startup");
const mongodWithParam = MongoRunner.runMongod(
    Object.merge(x509Options, {setParameter: {useInternalAuthzForX509: true}}),
);
const adminDBStartup = setupUsers(mongodWithParam);

withAdmin(adminDBStartup, (db) => {
    const startupResult = assert.commandWorked(db.runCommand({getParameter: 1, useInternalAuthzForX509: 1}));
    assert.eq(true, startupResult.useInternalAuthzForX509, "should be true when set at startup");

    jsTest.log.info("Testing X.509 auth with useInternalAuthzForX509=true (set at startup)");
    assertX509AuthSucceeds(mongodWithParam.port);
});

MongoRunner.stopMongod(mongodWithParam);
