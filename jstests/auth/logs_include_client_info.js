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

function checkAuthSuccess(element, index, array) {
    const log = JSON.parse(element);

    return log.id === 20250 && log.attr.principalName === "root" &&
        log.attr.authenticationDatabase === "admin" &&
        /(?:\d{1,3}\.){3}\d{1,3}:\d+/.test(log.attr.remote);
}

function checkSCRAMfail(element, index, array) {
    const log = JSON.parse(element);

    return log.id === 20249 && /SCRAM-SHA-\d+/.test(log.attr.mechanism) &&
        log.attr.principalName === "root" && log.attr.authenticationDatabase === "admin" &&
        /(?:\d{1,3}\.){3}\d{1,3}:\d+/.test(log.attr.remote);
}

assert(log.some(checkAuthSuccess));
assert(log.some(checkSCRAMfail));

MongoRunner.stopMongod(conn);
})();
