// Test whether a pinned cursor does not show up as an idle cursor in curOp.
// Then test and make sure a pinned cursor shows up in the operation object.
// @tags: [
//   requires_sharding,
// ]
(function() {
"use strict";
load("jstests/libs/pin_getmore_cursor.js");  // for "withPinnedCursor"

function runTest(cursorId, coll) {
    const db = coll.getDB();
    const adminDB = db.getSiblingDB("admin");
    // Test that active cursors do not show up as idle cursors.
    const idleCursors =
        adminDB
            .aggregate([
                {"$currentOp": {"localOps": true, "idleCursors": true, "allUsers": false}},
                {"$match": {"type": "idleCursor"}}
            ])
            .toArray();
    assert.eq(idleCursors.length, 0, tojson(idleCursors));
    // Test that an active cursor shows up in currentOp.
    const activeCursors =
        adminDB
            .aggregate([
                {"$currentOp": {"localOps": true, "idleCursors": false, "allUsers": false}},
                {"$match": {"cursor": {"$exists": true}}}
            ])
            .toArray();
    assert.eq(activeCursors.length, 1, tojson(activeCursors));
    const cursorObject = activeCursors[0].cursor;
    assert.eq(cursorObject.originatingCommand.find, coll.getName(), tojson(activeCursors));
    assert.eq(cursorObject.nDocsReturned, 2, tojson(activeCursors));
    assert.eq(cursorObject.tailable, false, tojson(activeCursors));
    assert.eq(cursorObject.awaitData, false, tojson(activeCursors));
}
const conn = MongoRunner.runMongod({});
let failPointName = "waitWithPinnedCursorDuringGetMoreBatch";
withPinnedCursor({
    conn: conn,
    sessionId: null,
    db: conn.getDB("test"),
    assertFunction: runTest,
    runGetMoreFunc: function(collName, cursorId) {
        assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName}));
    },
    failPointName: failPointName,
    assertEndCounts: true
});
MongoRunner.stopMongod(conn);

// Sharded test
failPointName = "waitAfterPinningCursorBeforeGetMoreBatch";
let st = new ShardingTest({shards: 2, mongos: 1});
withPinnedCursor({
    conn: st.s,
    sessionId: null,
    db: st.s.getDB("test"),
    assertFunction: runTest,
    runGetMoreFunc: function(collName, cursorId) {
        assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName}));
    },
    failPointName: failPointName,
    assertEndCounts: true
});
st.stop();
})();
