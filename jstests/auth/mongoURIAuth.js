// This tests that the shell successfully breaks down the URI and authenticates using
// the specified auth mechanism.

const runURIAuthTest = function (userMech, uriMech, authMechanism, regexMechanism) {
    const conn = MongoRunner.runMongod({
        auth: "",
        slowms: 30000, // Don't log slow operations to improve test reliability
    });
    const adminDB = conn.getDB("admin");

    adminDB.createUser({
        user: "u",
        pwd: "p",
        roles: ["root"],
    });
    adminDB.auth("u", "p");
    adminDB.setLogLevel(2, "command");

    if (userMech) {
        adminDB.createUser({
            user: "user",
            pwd: "password",
            roles: ["root"],
            mechanisms: [authMechanism],
        });
    } else {
        adminDB.createUser({
            user: "user",
            pwd: "password",
            roles: ["root"],
        });
    }

    let uri;

    if (uriMech) {
        uri = "mongodb://user:password@localhost:" + conn.port + "/admin?authMechanism=" + authMechanism;
    } else {
        uri = "mongodb://user:password@localhost:" + conn.port;
    }

    let shell = runMongoProgram("mongo", uri, "--eval", "db.getName()");
    assert.eq(shell, 0, "Should be able to connect with specified params.");

    const log = adminDB.runCommand({getLog: "global"});
    adminDB.logout();
    const matches = tojson(log.log).match(regexMechanism);
    assert(matches);
    assert.gte(matches.length, 2);

    MongoRunner.stopMongod(conn);
};

const SCRAM_SHA_256 = "SCRAM-SHA-256";
const SCRAM_SHA_1 = "SCRAM-SHA-1";

const SCRAM_SHA_256_regex = /saslStart[\\":\d,]*mechanism[\\":]*SCRAM-SHA-256/g;
const SCRAM_SHA_1_regex = /saslStart[\\":\d,]*mechanism[\\":]*SCRAM-SHA-1/g;

jsTestLog("Test that a mechanism specified in the URI is the chosen authentication method.");
runURIAuthTest(false, true, SCRAM_SHA_256, SCRAM_SHA_256_regex);

jsTestLog("Test that a mechanism specified in CreateUser() is the chosen authentication method.");
runURIAuthTest(true, false, SCRAM_SHA_1, SCRAM_SHA_1_regex);

jsTestLog("Test that SCRAM-SHA-256 is the default authentication method.");
runURIAuthTest(false, false, SCRAM_SHA_256, SCRAM_SHA_256_regex);
