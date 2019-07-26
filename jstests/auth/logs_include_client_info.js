// This test just checks that the success/failure messages for authentication include the IP
// address of the client attempting to authenticate.

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

const successRegex =
    /Successfully authenticated as principal root on admin from client (?:\d{1,3}\.){3}\d{1,3}:\d+/;
const failRegex =
    /SASL SCRAM-SHA-\d+ authentication failed for root on admin from client (?:\d{1,3}\.){3}\d{1,3}:\d+/;

assert(log.some((line) => successRegex.test(line)));
assert(log.some((line) => failRegex.test(line)));
MongoRunner.stopMongod(conn);
})();
