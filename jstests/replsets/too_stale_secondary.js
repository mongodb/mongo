/**
 * This test ensures that a secondary that has gone "too stale" (i.e. cannot find another node with
 * a common oplog point) will transition to RECOVERING state, stay in RECOVERING after restart, and
 * transition back to SECONDARY once it finds a sync source with a common oplog point.
 *
 * Note: This test requires persistence in order for a restarted node with a stale oplog to stay in
 * the RECOVERING state. A restarted node with an ephemeral storage engine will not have an oplog
 * upon restart, so will immediately resync.
 *
 * @tags: [requires_persistence]
 *
 * Replica Set Setup:
 *
 * Node 0 (PRIMARY)     : Small Oplog
 * Node 1 (SECONDARY)   : Large Oplog
 * Node 2 (SECONDARY)   : Small Oplog
 *
 * 1:  Insert one document on the primary (Node 0) and ensure it is replicated.
 * 2:  Stop node 2.
 * 3:  Wait until Node 2 is down.
 * 4:  Overflow the primary's oplog.
 * 5:  Stop Node 1 and restart Node 2.
 * 6:  Wait for Node 2 to transition to RECOVERING (it should be too stale).
 * 7:  Stop and restart Node 2.
 * 8:  Wait for Node 2 to transition to RECOVERING (its oplog should remain stale after restart).
 * 9:  Restart Node 1, which should have the full oplog history.
 * 10: Wait for Node 2 to leave RECOVERING and transition to SECONDARY.
 *
 */

(function() {
    load('jstests/replsets/rslib.js');

    "use strict";

    function getFirstOplogEntry(conn) {
        return conn.getDB('local').oplog.rs.find().sort({$natural: 1}).limit(1)[0];
    }

    /**
     * Overflows the oplog of a given node.
     *
     * To detect oplog overflow, we continuously insert large documents until we
     * detect that the first entry of the oplog is no longer the same as when we started. This
     * implies that the oplog attempted to grow beyond its maximum size i.e. it
     * has overflowed/rolled over.
     *
     * Each document will be inserted with a writeConcern given by 'writeConcern'.
     *
     */
    function overflowOplog(conn, db, writeConcern) {
        var firstOplogEntry = getFirstOplogEntry(primary);
        var collName = "overflow";

        // Keep inserting large documents until the oplog rolls over.
        const largeStr = new Array(32 * 1024).join('aaaaaaaa');
        while (bsonWoCompare(getFirstOplogEntry(conn), firstOplogEntry) === 0) {
            assert.writeOK(
                db[collName].insert({data: largeStr}, {writeConcern: {w: writeConcern}}));
        }
    }

    var testName = "too_stale_secondary";

    var smallOplogSizeMB = 1;
    var bigOplogSizeMB = 1000;

    // Node 0 is given a small oplog so we can overflow it. Node 1's large oplog allows it to store
    // all entries comfortably without overflowing, so that Node 2 can eventually use it as a sync
    // source after it goes too stale.
    var replTest = new ReplSetTest({
        name: testName,
        nodes: [
            {oplogSize: smallOplogSizeMB},
            {oplogSize: bigOplogSizeMB},
            {oplogSize: smallOplogSizeMB}
        ]
    });

    var nodes = replTest.startSet();
    replTest.initiate({
        _id: testName,
        members: [
            {_id: 0, host: nodes[0].host},
            {_id: 1, host: nodes[1].host, priority: 0},
            {_id: 2, host: nodes[2].host, priority: 0}
        ]
    });

    var dbName = testName;
    var collName = "test";

    jsTestLog("Wait for Node 0 to become the primary.");
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

    var primary = replTest.getPrimary();
    var primaryTestDB = primary.getDB(dbName);

    jsTestLog("1: Insert one document on the primary (Node 0) and ensure it is replicated.");
    assert.writeOK(primaryTestDB[collName].insert({a: 1}, {writeConcern: {w: 3}}));

    jsTestLog("2: Stop Node 2.");
    replTest.stop(2);

    jsTestLog("3: Wait until Node 2 is down.");
    replTest.waitForState(replTest.nodes[2], ReplSetTest.State.DOWN);

    var firstOplogEntryNode1 = getFirstOplogEntry(replTest.nodes[1]);

    jsTestLog("4: Overflow the primary's oplog.");
    overflowOplog(primary, primaryTestDB, 2);

    // Make sure that Node 1's oplog didn't overflow.
    assert.eq(firstOplogEntryNode1,
              getFirstOplogEntry(replTest.nodes[1]),
              "Node 1's oplog overflowed unexpectedly.");

    jsTestLog("5: Stop Node 1 and restart Node 2.");
    replTest.stop(1);
    replTest.restart(2);

    jsTestLog("6: Wait for Node 2 to transition to RECOVERING (it should be too stale).");
    replTest.waitForState(replTest.nodes[2], ReplSetTest.State.RECOVERING);

    jsTestLog("7: Stop and restart Node 2.");
    replTest.stop(2);
    replTest.restart(2);

    jsTestLog(
        "8: Wait for Node 2 to transition to RECOVERING (its oplog should remain stale after restart)");
    replTest.waitForState(replTest.nodes[2], ReplSetTest.State.RECOVERING);

    jsTestLog("9: Restart Node 1, which should have the full oplog history.");
    replTest.restart(1);

    jsTestLog("10: Wait for Node 2 to leave RECOVERING and transition to SECONDARY.");
    replTest.waitForState(replTest.nodes[2], ReplSetTest.State.SECONDARY);

    replTest.stopSet();
}());