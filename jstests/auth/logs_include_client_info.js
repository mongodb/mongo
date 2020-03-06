// This test just checks that the success/failure messages for authentication include the IP
// address of the client attempting to authenticate.
load("jstests/libs/logv2_helpers.js");

(function() {
const conn = MongoRunner.runMongod({auth: ""});
const admin = conn.getDB("admin");

admin.createUser({
    user: "root",
    pwd: "root",
    roles: ["root"],
});

assert(admin.auth("root", "root"));

const failConn = new Mongo(conn.host);
failConn.getDB("admin").auth("root", "toot");

const log = assert.commandWorked(admin.runCommand({getLog: "global"})).log;

if (isJsonLog(conn)) {
    function checkAuthSuccess(element, index, array) {
        const log = JSON.parse(element);

        return log.id === 20250 && log.attr.principalName === "root" &&
            log.attr.authenticationDatabase === "admin" &&
            /(?:\d{1,3}\.){3}\d{1,3}:\d+/.test(log.attr.client);
    }

    function checkSCRAMfail(element, index, array) {
        const log = JSON.parse(element);

        return log.id === 20249 && /SCRAM-SHA-\d+/.test(log.attr.mechanism) &&
            log.attr.principalName === "root" && log.attr.authenticationDatabase === "admin" &&
            /(?:\d{1,3}\.){3}\d{1,3}:\d+/.test(log.attr.client);
    }

    assert(log.some(checkAuthSuccess));
    assert(log.some(checkSCRAMfail));
} else {
    const successRegex =
        /Successfully authenticated as principal root on admin from client (?:\d{1,3}\.){3}\d{1,3}:\d+/;
    const failRegex =
        /SASL SCRAM-SHA-\d+ authentication failed for root on admin from client (?:\d{1,3}\.){3}\d{1,3}:\d+/;

    assert(log.some((line) => successRegex.test(line)));
    assert(log.some((line) => failRegex.test(line)));
}

MongoRunner.stopMongod(conn);
})();
