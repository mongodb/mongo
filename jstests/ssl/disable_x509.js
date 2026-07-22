// Tests enabling and disabling the MONGODB-X509 authentication mechanism, for both mongod and
// mongos. Both the standalone and sharded cases are exercised to ensure the allow-list is honored
// on every entry point.
//
// The subject DN is written in RFC 2253 order ("CN=client,...,C=US") to match the order in
// which the SASL X.509 path derives the principal from the TLS certificate. Using the legacy
// LDAP order ("C=US,...,CN=client") would cause user lookup to fail for a reason unrelated to
// the mechanism allow-list, masking whether the allow-list check itself is working.
//
// @tags: [requires_sharding]

import {ShardingTest} from "jstests/libs/shardingtest.js";

const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

const x509Options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
    tlsAllowInvalidHostnames: "",
};

// ---------------------------------------------------------------------------
// Standalone helpers
// ---------------------------------------------------------------------------

// Starts a standalone mongod restricted to the given comma-separated mechanisms, creates a
// SCRAM admin user (via the localhost exception) and the $external X.509 user, then returns
// the connection. SCRAM-SHA-256 must be included in |mechanisms| for admin setup to work.
function startMongodWithMechanisms(mechanisms) {
    const conn = MongoRunner.runMongod(
        Object.merge(x509Options, {
            auth: "",
            setParameter: {authenticationMechanisms: mechanisms},
        }),
    );

    const admin = conn.getDB("admin");
    admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
    assert(admin.auth("admin", "pwd"), "admin SCRAM auth failed during setup");

    conn.getDB("$external").createUser({
        user: CLIENT_USER,
        roles: [{role: "readWriteAnyDatabase", db: "admin"}],
    });
    admin.logout();

    return conn;
}

// ---------------------------------------------------------------------------
// Sharding helpers
// ---------------------------------------------------------------------------

// Starts a ShardingTest whose mongos is restricted to the given mechanisms, creates a SCRAM
// admin user and the $external X.509 user via mongos, then returns the ShardingTest object.
function startMongosWithMechanisms(mechanisms) {
    const st = new ShardingTest({
        shards: 0,
        other: {
            keyFile: "jstests/libs/key1",
            // Config RS nodes must also use TLS so that mongos can reach them.
            configOptions: x509Options,
            mongosOptions: Object.merge(x509Options, {
                setParameter: {authenticationMechanisms: mechanisms},
            }),
        },
    });

    const admin = st.s.getDB("admin");
    admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
    assert(admin.auth("admin", "pwd"), "admin SCRAM auth failed during mongos setup");

    st.s.getDB("$external").createUser({
        user: CLIENT_USER,
        roles: [{role: "readWriteAnyDatabase", db: "admin"}],
    });
    admin.logout();

    return st;
}

// ---------------------------------------------------------------------------
// Standalone: mongod
// ---------------------------------------------------------------------------

jsTest.log.info("mongod — X509 NOT in allow-list: MONGODB-X509 auth must be rejected");
{
    const conn = startMongodWithMechanisms("SCRAM-SHA-256");
    assert(
        !conn.getDB("$external").auth({user: CLIENT_USER, mechanism: "MONGODB-X509"}),
        "MONGODB-X509 authentication succeeded on mongod even though it is excluded from authenticationMechanisms",
    );
    MongoRunner.stopMongod(conn);
}

jsTest.log.info("mongod — X509 IN allow-list: MONGODB-X509 auth must succeed");
{
    const conn = startMongodWithMechanisms("SCRAM-SHA-256,MONGODB-X509");
    const external = conn.getDB("$external");
    assert(
        external.auth({user: CLIENT_USER, mechanism: "MONGODB-X509"}),
        "MONGODB-X509 authentication failed on mongod even though it is in authenticationMechanisms",
    );
    external.logout();
    MongoRunner.stopMongod(conn);
}

// ---------------------------------------------------------------------------
// Sharded: mongos
// ---------------------------------------------------------------------------

if (TestData.configShard) {
    // Config shard mode requires at least one shard; skip the sharded cases.
    jsTest.log.info("Skipping mongos cases: configShard mode requires at least one shard");
    quit();
}

jsTest.log.info("mongos — X509 NOT in allow-list: MONGODB-X509 auth must be rejected");
{
    const st = startMongosWithMechanisms("SCRAM-SHA-256");
    assert(
        !st.s.getDB("$external").auth({user: CLIENT_USER, mechanism: "MONGODB-X509"}),
        "MONGODB-X509 authentication succeeded on mongos even though it is excluded from authenticationMechanisms",
    );
    st.stop();
}

jsTest.log.info("mongos — X509 IN allow-list: MONGODB-X509 auth must succeed");
{
    const st = startMongosWithMechanisms("SCRAM-SHA-256,MONGODB-X509");
    const external = st.s.getDB("$external");
    assert(
        external.auth({user: CLIENT_USER, mechanism: "MONGODB-X509"}),
        "MONGODB-X509 authentication failed on mongos even though it is in authenticationMechanisms",
    );
    external.logout();
    st.stop();
}
