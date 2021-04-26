/**
 * Tests that logout emits a deprecation warning once.
 *
 * @tags: [
 *   requires_auth,
 *   requires_sharding,
 *   requires_non_retryable_commands,
 *   requires_fcv_50
 * ]
 */
(function() {
"use strict";

function runTest(conn) {
    const logId = 5626600;
    const logAttr = {};

    const admin = conn.getDB("admin");
    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth('admin', 'pass'));

    admin.createUser({user: 'test', pwd: 'pass', roles: []});

    const logoutConn = new Mongo(conn.host);
    const logoutDB = logoutConn.getDB('admin');

    assert(logoutDB.auth('test', 'pass'));
    assert.commandWorked(logoutDB.runCommand({logout: 1}));
    assert(checkLog.checkContainsOnceJson(admin, logId, logAttr));

    // We don't emit the message a second time, so there's still just one entry.
    assert(logoutDB.auth('test', 'pass'));
    assert.commandWorked(logoutDB.runCommand({logout: 1}));
    assert(checkLog.checkContainsOnceJson(admin, logId, logAttr));
}

{
    jsTest.log("Running standalone test");
    const m = MongoRunner.runMongod({auth: ""});
    runTest(m);
    MongoRunner.stopMongod(m);
}

{
    jsTest.log("Running sharded test");
    const st =
        new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
    runTest(st.s0);
    st.stop();
}
})();
