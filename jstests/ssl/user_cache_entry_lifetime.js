// Test that we can safely use entries in the user cache created by old connections.

const SERVER_CERT = "jstests/libs/server.pem";
const CA_CERT = "jstests/libs/ca.pem";
const CLIENT_CERT = "jstests/libs/client_roles.pem";

function runTest(port) {
    // Run given test function in a parallel shell.
    const runShell = function(func) {
        const res = runMongoProgram("mongo",
                                    "--host",
                                    "localhost",
                                    "--port",
                                    port,
                                    "--tls",
                                    "--tlsCAFile",
                                    CA_CERT,
                                    "--tlsCertificateKeyFile",
                                    CLIENT_CERT,
                                    "--eval",
                                    `(${func.toString()})();`);

        assert.eq(0, res, "Connection attempt failed");
    };

    // Log in as the user for the first time, then kill the connection. This will store a user
    // object in the cache which references the SSLPeerInfo object stored on the now-dead session.
    jsTest.log("Creating initial user cache entry");
    runShell(() => {
        let ret = db.getSiblingDB("$external").auth({
            mechanism: "MONGODB-X509",
            user:
                "CN=Kernel Client Peer Role,OU=Kernel Users,O=MongoDB,L=New York City,ST=New York,C=US"
        });
        assert.eq(ret, 1, "Auth failed");
    });
    jsTest.log("Attempt to use old user entry");
    runShell(`() => {
        // When we log in, we acquire the aforementioned user object.
        let ret = db.getSiblingDB("$external").auth({
            mechanism: "MONGODB-X509",
            user: "CN=Kernel Client Peer Role,OU=Kernel Users,O=MongoDB,L=New York City,ST=New York,C=US"
        });
        assert.eq(ret, 1, "Auth failed");

        // Invalidate the cache by creating a role as an admin user
        const res = runMongoProgram("mongo",
            "--host",
            "localhost",
            "--port",
            ${port},
            "--tls",
            "--tlsCAFile",
            "${CA_CERT}",
            "--tlsCertificateKeyFile",
            "${CLIENT_CERT}",
            "--eval",
            \`const admin = db.getSiblingDB('admin');
              assert(admin.auth('admin', 'admin'));
              assert.commandWorked(admin.runCommand({ createRole: "roleabc", roles: [], privileges: [] }));
            \`);
        assert.eq(0, res);

        // Now reacquire the user. Because our user object was just invalidated, we will use the SSLPeerInfo on the acquired user object to generate a user cache key.
        db["test"].find({});
    }`);
}

const setup = function(conn) {
    // Create an admin user, which we will later use to invalidate the user cache.
    const admin = conn.getDB('admin');
    admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
    assert(admin.auth('admin', 'admin'));
};

let mongo = MongoRunner.runMongod(
    {tlsMode: "requireTLS", tlsCertificateKeyFile: SERVER_CERT, tlsCAFile: CA_CERT, auth: ""});
jsTest.log("Setup");
setup(mongo);

jsTest.log("Test");
runTest(mongo.port);

MongoRunner.stopMongod(mongo);
