// Test X509 auth when --tlsAllowInvalidCertificates is enabled

const CLIENT_NAME = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";
const CLIENT_CERT = "jstests/libs/client.pem";
const SERVER_CERT = "jstests/libs/server.pem";
const CA_CERT = "jstests/libs/ca.pem";
const SELF_SIGNED_CERT = "jstests/libs/client-self-signed.pem";

function hasX509AuthSucceeded(conn) {
    if (checkLog.checkContainsOnce(conn, "No verified subject name available from client")) {
        return false;
    }

    if (checkLog.checkContainsOnce(conn, "Successfully authenticated")) {
        return true;
    }
    print("Not yet clear what was the result...");
    return null;
}

function testClient(cert, name, shouldSucceed) {
    print("Starting mongod...");
    const conn = MongoRunner.runMongod({
        auth: "",
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
        tlsAllowInvalidCertificates: "",
    });

    print("Creating admin user...");
    const admin = conn.getDB("admin");
    admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
    admin.auth("admin", "admin");

    print("Creating external user...");
    const external = conn.getDB("$external");
    external.createUser({user: CLIENT_NAME, roles: [{"role": "readWrite", "db": "test"}]});

    let auth = {mechanism: "MONGODB-X509"};
    if (name !== null) {
        auth.user = name;
    }

    print("Running mongo shell script...");
    if (!shouldSucceed) {
        print("Note: following shell command is expected to fail");
    }

    const script = "assert(db.getSiblingDB('$external').auth(" + tojson(auth) + "));";
    const exitCode = runMongoProgram(
        "mongo",
        "--tls",
        "--tlsAllowInvalidHostnames",
        "--tlsCertificateKeyFile",
        cert,
        "--tlsCAFile",
        CA_CERT,
        "--port",
        conn.port,
        "--eval",
        script,
    );

    print("Analyzing results...");
    assert.eq(shouldSucceed, exitCode === 0, "exitCode = " + tojson(exitCode));
    assert.soon(
        () => hasX509AuthSucceeded(admin) !== null,
        "can not find in mongod logs whether it succeeded to authenticate",
        15000,
    );
    assert.eq(shouldSucceed, hasX509AuthSucceeded(admin));

    print("Stopping mongod...");
    MongoRunner.stopMongod(conn);
}

testClient(CLIENT_CERT, CLIENT_NAME, true);
testClient(SELF_SIGNED_CERT, CLIENT_NAME, false);
testClient(CLIENT_CERT, null, true);
testClient(SELF_SIGNED_CERT, null, false);
