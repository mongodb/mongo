/**
 * Tests that oplog writes are forbidden on replica set members. In standalone mode, it is permitted
 * to insert oplog entries.
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let conn = rst.getPrimary();
    assert.writeOK(conn.getDB("test").coll.insert({_id: 0, a: 0}));

    let oplog = conn.getDB("local").oplog.rs;

    // Construct a valid oplog entry.
    const lastOplogEntry = oplog.find().sort({ts: -1}).limit(1).toArray()[0];
    const highestTS = lastOplogEntry.ts;
    const toInsert = Object.extend(lastOplogEntry, {
        op: "u",
        ns: "test.coll",
        o: {$set: {a: 1}},
        o2: {_id: 0},
        ts: Timestamp(highestTS.getTime(), highestTS.getInc() + 1)
    });

    jsTestLog("Test that oplog writes are banned when replication is enabled.");
    assert.writeErrorWithCode(oplog.insert(toInsert), ErrorCodes.InvalidNamespace);

    jsTestLog("Restart the node in standalone mode.");
    rst.stop(0, undefined /*signal*/, undefined /*opts*/, {forRestart: true});
    conn = rst.start(0, {noReplSet: true, noCleanData: true});

    jsTestLog("Test that oplog writes are permitted in standalone mode.");
    assert.writeOK(conn.getDB("local").oplog.rs.insert(toInsert));

    rst.stopSet();
}());
