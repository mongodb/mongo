// Basic tests for the $listLocalCursors aggregation stage.
//
// $listLocalCursors relies on in-memory state, which may not survive failovers.
// Uses features that require featureCompatibilityVersion 3.6.
// @tags: [does_not_support_stepdowns, requires_fcv36]

(function() {
    "use strict";

    const admin = db.getSisterDB("admin");
    const countAllMatchingLocalCursors = function(match) {
        return admin
            .aggregate([
                {"$listLocalCursors": {}},
                {"$match": match},
                {"$count": "matches"},
            ])
            .next()
            .matches;
    };

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

    // Ensure that the cache now contains the session and is visible by admin.
    const count = countAllMatchingLocalCursors({
        "ns": "listAllLocalCursors.data",
        "$or": [
            {
              "id": cursorIdWithSession,
              "lsid.id": session._serverSession.handle.getId().id,
            },
            {
              "id": cursorIdWithoutSession,
            },
        ],
    });
    assert.eq(count, 2);

    assert.commandWorked(testDbWithSession.runCommand(
        {killCursors: "data", cursors: [cursorIdWithSession, cursorIdWithoutSession]}));
    session.endSession();
})();
