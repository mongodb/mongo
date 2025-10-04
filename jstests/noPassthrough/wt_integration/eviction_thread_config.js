/**
 * Tests that the wiredTigerEvictionThreadsMin and wiredTigerEvictionThreadsMax configs can be
 * set at startup and runtime.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
const startupValue = 5;
const conn = MongoRunner.runMongod({
    setParameter: {
        wiredTigerEvictionThreadsMin: startupValue,
        wiredTigerEvictionThreadsMax: startupValue,
    },
});

const db = conn.getDB("test");
const admin = conn.getDB("admin");
admin.setLogLevel(1);

const runtimeValue = 3;

jsTestLog("minThreads: ", runtimeValue);
assert.commandWorked(db.adminCommand({setParameter: 1, wiredTigerEvictionThreadsMin: runtimeValue}));

jsTestLog("maxThreads: ", runtimeValue);
assert.commandWorked(db.adminCommand({setParameter: 1, wiredTigerEvictionThreadsMax: runtimeValue}));

// check startup value was set and is being replaced by the new runtime value
checkLog.contains(db, `"newValue":"${runtimeValue}.0","oldValue":"${startupValue}"`);
checkLog.containsJson(conn, 10724800); // Log ID: call to _conn->reconfigure
MongoRunner.stopMongod(conn);
