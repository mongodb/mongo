// Test whether a pinned cursor does not show up as an idle cursor in curOp.
(function() {
    "use strict";
    load("jstests/libs/pin_getmore_cursor.js");  // for "withPinnedCursor"

    function runTest(cursorId, coll) {
        const db = coll.getDB();
        const adminDB = db.getSiblingDB("admin");
        const result = adminDB
                           .aggregate([
                               {"$currentOp": {"idleCursors": true, "allUsers": false}},
                               {"$match": {"type": "idleCursor"}}
                           ])
                           .toArray();
        assert.eq(result.length, 0, tojson(result));
    }

    const conn = MongoRunner.runMongod({});
    const failPointName = "waitAfterPinningCursorBeforeGetMoreBatch";
    withPinnedCursor({
        conn: conn,
        assertFunction: runTest,
        runGetMoreFunc: function() {
            const response =
                assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName}));
        },
        failPointName: failPointName
    });
    MongoRunner.stopMongod(conn);

})();
