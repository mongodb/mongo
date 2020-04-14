/**
 * Test that we can downgrade a replica set shard server.
 *
 * This exercises a behavior that might occur, for example, after we have added a shard replica set
 * to a cluster that is already in a downgraded FCV.
 */
(function() {
"use strict";

let testName = "downgrade_replset_shard";
let dbpath = MongoRunner.dataPath + "-" + testName;
let conn = MongoRunner.runMongod({dbpath: dbpath, replSet: testName, shardsvr: ""});
MongoRunner.awaitConnection(conn.pid, conn.port);
assert.commandWorked(conn.adminCommand({replSetInitiate: 1}));

// Wait for the node to be elected.
assert.soonNoExcept(() => {
    return conn.adminCommand({ismaster: 1}).ismaster;
});

jsTestLog("Stopping node.");
MongoRunner.stopMongod(conn, 15, {noCleanData: true});

jsTestLog("Restarting node in last-stable binary version.");
conn = MongoRunner.runMongod({
    dbpath: dbpath,
    replSet: testName,
    shardsvr: "",
    binVersion: "last-stable",
    restart: true,
    noCleanData: true
});
MongoRunner.awaitConnection(conn.pid, conn.port);

jsTestLog("Stopping node finally.");
MongoRunner.stopMongod(conn);
})();
