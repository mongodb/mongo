// Validate that write blocking mode prevents reaping of user, but not system, TTL indexes
// @tags: [
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_fcv_60,
//   requires_non_retryable_commands,
//   requires_replication,
// ]

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

// Test on replset primary
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            "ttlMonitorSleepSecs": 1,
        }
    }
});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

function runTest(conn, testCase) {
    const admin = conn.getDB("admin");

    // Make sure the TTLMonitor is disabled, while not holding any global locks
    const pauseTtl = configureFailPoint(primary, 'hangTTLMonitorBetweenPasses');
    pauseTtl.wait();

    function runTTLMonitor() {
        const ttlPass = admin.serverStatus().metrics.ttl.passes;
        assert.commandWorked(
            admin.runCommand({configureFailPoint: "hangTTLMonitorBetweenPasses", mode: {skip: 1}}));
        assert.soon(() => admin.serverStatus().metrics.ttl.passes >= ttlPass + 1,
                    "TTL monitor didn't run before timing out.");
    }

    // Set up data and TTL indexes
    for (const target of testCase) {
        target.db = conn.getDB(target.dbname);
        target.col = target.db.getCollection(target.colName);

        target.col.insertOne({"createdAt": new Date(), "logEvent": 2, "logMessage": "Success!"});

        assert.commandWorked(target.col.createIndex({"createdAt": 1}, {expireAfterSeconds: 0}));
    }

    // Enable global write block mode
    assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: true}));
    for (const target of testCase) {
        assert.eq(1, target.col.count());
    }

    // Run the TTLMonitor, and expect user collections to remain unreaped
    runTTLMonitor();
    for (const target of testCase) {
        if (target.expectReap) {
            assert.eq(0, target.col.count());
        } else {
            assert.eq(1, target.col.count());
            checkLog.containsJson(conn, 5400703, {
                "error": {
                    "code": ErrorCodes.UserWritesBlocked,
                    "codeName": "UserWritesBlocked",
                    "errmsg": "User writes blocked"
                }
            });
        }
    }

    // Disable write blocking, then run the TTLMonitor, expecting it to reap all collections
    assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: false}));
    runTTLMonitor();

    for (const target of testCase) {
        // All documents should be reaped now
        assert.soon(() => target.col.count() == 0);

        // Finally, cleanup the collection
        if (target.colName != "system.js") {
            assert(target.col.drop());
        }
    }
}

runTest(primary, [
    // Collections in a regular database should not be reaped
    {dbname: "db", colName: "user_write_blocking_ttl_index", expectReap: false},
    // Not even system collections in a regular database should be reaped
    {dbname: "db2", colName: "system.js", expectReap: false},
    // Collections in system databases should be reaped
    {dbname: "admin", colName: "user_write_blocking_ttl_index", expectReap: true},
    {dbname: "config", colName: "user_write_blocking_ttl_index", expectReap: true},
    {dbname: "local", colName: "user_write_blocking_ttl_index", expectReap: true}
]);
rst.stopSet();
})();
