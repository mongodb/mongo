/**
 * Test that a node that is shutdown without running replication recovery does not downgrade to
 * 3.6 compatible data files, even when FCV is 3.6.
 *
 * @tags: [requires_wiredtiger, requires_persistence, requires_replication]
 */

// Steps:
// 1) Setup a one node replica set with `--shardsvr`. Shards are born in FCV 3.6
// 2) Initiate the replica set.
// 3) Wait for the first stable checkpoint.
// 4) Set FCV to 4.0
// 5) Insert a document.
// 6) Hard crash the server. The checkpoint does not contain (4) nor (5), but
//    the oplog does.
// 7) Start the node as a standalone. Confirm the write at (5) is missing.
// 8) Shutdown the node. Run `--repair`. This will not play oplog recovery.
//    When this process shuts down, it must not downgrade the files.
// 9) Restart the node as a replica set/shard. Assert oplog recovery went well
//    by observing the write at (5).
(function() {
    "use strict";

    const name = "rs";
    let conn = MongoRunner.runMongod(
        {binVersion: "latest", shardsvr: "", replSet: name, syncdelay: "600"});

    assert.neq(conn, null, "mongod was unable to start up");

    let options = conn.fullOptions;

    assert.commandWorked(conn.adminCommand({replSetInitiate: 1}));
    assert.soon(function() {
        let status = assert.commandWorked(conn.adminCommand("replSetGetStatus"));
        return status["lastStableCheckpointTimestamp"].t > 0;
    });

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    assert.commandWorked(
        conn.getDB("foo")["bar"].insert({_id: 0}, {writeConcern: {w: 1, j: true}}));

    jsTestLog("Oplog entries:");
    let cursor = conn.getDB("local").oplog.rs.find();
    while (cursor.hasNext()) {
        printjson(cursor.next());
    }

    MongoRunner.stopMongod(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

    jsTestLog("Running as standalone.");
    options.shardsvr = undefined;
    options.replSet = undefined;
    options.restart = true;
    conn = MongoRunner.runMongod(options);

    assert.eq(0, conn.getDB("foo")["bar"].find({_id: 0}).itcount());
    MongoRunner.stopMongod(conn);

    jsTestLog("Restarting after standalone with replication.");
    options.shardsvr = "";
    options.replSet = name;

    conn = MongoRunner.runMongod(options);
    assert.neq(conn, null, "mongod was unable to start up");

    assert.soon(function() {
        return conn.adminCommand("ismaster")["ismaster"];
    });
    assert.eq(1, conn.getDB("foo")["bar"].find({_id: 0}).itcount());
    MongoRunner.stopMongod(conn);

    jsTestLog("Running repair. The process will automatically exit when complete.");
    options.shardsvr = undefined;
    options.replSet = undefined;
    options.repair = '';
    MongoRunner.runMongod(options);

    jsTestLog("Restarting after repair with replication.");
    options.shardsvr = '';
    options.replSet = name;
    options.repair = undefined;

    conn = MongoRunner.runMongod(options);
    assert.neq(conn, null, "mongod was unable to start up");

    assert.soon(function() {
        return conn.adminCommand("ismaster")["ismaster"];
    });
    assert.eq(1, conn.getDB("foo")["bar"].find({_id: 0}).itcount());
    MongoRunner.stopMongod(conn);
})();
