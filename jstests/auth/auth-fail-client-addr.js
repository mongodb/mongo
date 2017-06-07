// Verify that client IP address is included in MONGODB-CR auth failure response

function runTest(m) {
    db = m.getDB("test");
    db.auth({user: "root", pwd: "periwinkle", mechanism: "MONGODB-CR"});

    assert.soon(function() {
        var log = cat(m.fullOptions.logFile);
        return /Failed to authenticate root@test from client 127\.0\.0\.1:\d+/.test(log);
    }, "Authentication failure message not encountered or missing client IP", 30 * 1000, 5 * 1000);
}

print("START auth-fail-client-addr.js");
runTest(MongoRunner.runMongod({useHostname: false, useLogFiles: true}));
print("SUCCESS auth-fail-client-addr.js");
