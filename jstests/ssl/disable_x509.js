// Test enabling and disabling the MONGODB-X509 auth mech

var CLIENT_USER = "C=US,ST=New York,L=New York City,O=MongoDB,OU=KernelUser,CN=client";

var conn = MongoRunner.runMongod({
    smallfiles: "",
    auth: "",
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem"
});

// Find out if this build supports the authenticationMechanisms startup parameter.
// If it does, restart with and without the MONGODB-X509 mechanisms enabled.
var cmdOut = conn.getDB('admin').runCommand({getParameter: 1, authenticationMechanisms: 1});
if (cmdOut.ok) {
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(
        {restart: conn, setParameter: "authenticationMechanisms=MONGODB-X509"});
    external = conn.getDB("$external");

    // Add user using localhost exception
    external.createUser({
        user: CLIENT_USER,
        roles: [
            {'role': 'userAdminAnyDatabase', 'db': 'admin'},
            {'role': 'readWriteAnyDatabase', 'db': 'admin'}
        ]
    });

    // Localhost exception should not be in place anymore
    assert.throws(function() {
        test.foo.findOne();
    }, [], "read without login");

    assert(external.auth({user: CLIENT_USER, mechanism: 'MONGODB-X509'}),
           "authentication with valid user failed");
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod(
        {restart: conn, setParameter: "authenticationMechanisms=SCRAM-SHA-1"});
    external = conn.getDB("$external");

    assert(!external.auth({user: CLIENT_USER, mechanism: 'MONGODB-X509'}),
           "authentication with disabled auth mechanism succeeded");
}
