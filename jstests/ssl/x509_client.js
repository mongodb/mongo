// Check if this build supports the authenticationMechanisms startup parameter.

import {ShardingTest} from "jstests/libs/shardingtest.js";

const SERVER_CERT = "jstests/libs/server.pem";
const CA_CERT = "jstests/libs/ca.pem";

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

    // Check that there's a "Successfully authenticated" message that includes the client IP
    const log = assert.commandWorked(external.getSiblingDB("admin").runCommand({getLog: "global"})).log;

    function checkAuthSuccess(element /*, index, array*/) {
        const logJson = JSON.parse(element);

        return (
            logJson.id === 5286306 &&
            logJson.attr.user === CLIENT_USER &&
            logJson.attr.db === "$external" &&
            /(?:\d{1,3}\.){3}\d{1,3}:\d+/.test(logJson.attr.client)
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
