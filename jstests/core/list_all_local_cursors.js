// $listLocalCursors relies on in-memory state, which may not survive failovers.
// @tags: [
//   does_not_support_stepdowns,
//   uses_testing_only_commands,
// ]

// Basic tests for the $listLocalCursors aggregation stage.

(function() {
    "use strict";

    const admin = db.getSisterDB("admin");
    function listAllCursorsWithId(cursorId) {
        return admin
            .aggregate([
                {"$listLocalCursors": {}},
                {"$match": {"id": cursorId}},
            ])
            .toArray();
    }

    let session = db.getMongo().startSession();
    let testDb = db.getSisterDB("listAllLocalCursors");
    let testDbWithSession = session.getDatabase("listAllLocalCursors");
    testDb.data.drop();
    assert.writeOK(testDb.data.insert({a: 1}));
    assert.writeOK(testDb.data.insert({a: 1}));
    let cursorIdWithSession =
        assert.commandWorked(testDbWithSession.runCommand({find: "data", batchSize: 0})).cursor.id;
    let cursorIdWithoutSession =
        assert.commandWorked(testDb.runCommand({find: "data", batchSize: 0})).cursor.id;

    // Verify that we correctly list the cursor which is outside of a session.
    let foundCursors = listAllCursorsWithId(cursorIdWithoutSession);
    assert.eq(foundCursors.length, 1, tojson(foundCursors));
    assert.eq(foundCursors[0].ns, "listAllLocalCursors.data", tojson(foundCursors));
    assert.eq(foundCursors[0].id, cursorIdWithoutSession, tojson(foundCursors));

    // Verify that we correctly list the cursor which is inside of a session.
    foundCursors = listAllCursorsWithId(cursorIdWithSession);
    assert.eq(foundCursors.length, 1, tojson(foundCursors));
    assert.eq(foundCursors[0].ns, "listAllLocalCursors.data", tojson(foundCursors));
    assert.eq(foundCursors[0].id, cursorIdWithSession, tojson(foundCursors));
    assert(foundCursors[0].hasOwnProperty("lsid"), tojson(foundCursors));
    assert.eq(
        foundCursors[0].lsid.id, session._serverSession.handle.getId().id, tojson(foundCursors));

    assert.commandWorked(testDbWithSession.runCommand(
        {killCursors: "data", cursors: [cursorIdWithSession, cursorIdWithoutSession]}));
    session.endSession();
})();
