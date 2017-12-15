// Basic tests for the $listSessions aggregation stage.
//
// Sessions are asynchronously flushed to disk, so a stepdown immediately after calling
// startSession may cause this test to fail to find the returned sessionId.
// Uses features that require featureCompatibilityVersion 3.6.
// @tags: [does_not_support_stepdowns, requires_fcv36]

(function() {
    'use strict';
    load('jstests/aggregation/extras/utils.js');

    const admin = db.getSiblingDB('admin');
    const config = db.getSiblingDB('config');
    const pipeline = [{'$listSessions': {}}];
    function listSessions() {
        return config.system.sessions.aggregate(pipeline);
    }

    // Start a new session and capture its sessionId.
    const myid = assert.commandWorked(admin.runCommand({startSession: 1})).id.id;
    assert(myid !== undefined);

    // Sync cache to collection and ensure it arrived.
    assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));
    var resultArrayMine;
    assert.soon(function() {
        const resultArray = listSessions().toArray();
        if (resultArray.length < 1) {
            return false;
        }
        resultArrayMine = resultArray
                              .map(function(sess) {
                                  return sess._id;
                              })
                              .filter(function(id) {
                                  return 0 == bsonWoCompare({x: id.id}, {x: myid});
                              });
        return resultArrayMine.length == 1;
    }, "Failed to locate session in collection");

    // Try asking for the session by username.
    const myusername = (function() {
        if (0 == bsonWoCompare({x: resultArrayMine[0].uid}, {x: computeSHA256Block("")})) {
            // Code for "we're running in no-auth mode"
            return {user: "", db: ""};
        }
        const connstats = assert.commandWorked(db.runCommand({connectionStatus: 1}));
        const authUsers = connstats.authInfo.authenticatedUsers;
        assert(authUsers !== undefined);
        assert.eq(authUsers.length, 1);
        assert(authUsers[0].user !== undefined);
        assert(authUsers[0].db !== undefined);
        return {user: authUsers[0].user, db: authUsers[0].db};
    })();
    function listMySessions() {
        return config.system.sessions.aggregate([{'$listSessions': {users: [myusername]}}]);
    }
    const myArray = listMySessions()
                        .toArray()
                        .map(function(sess) {
                            return sess._id;
                        })
                        .filter(function(id) {
                            return 0 == bsonWoCompare({x: id.id}, {x: myid});
                        });
    assert.eq(0, bsonWoCompare(myArray, resultArrayMine));

    // Make sure pipelining other collections fail.
    assertErrorCode(admin.system.collections, pipeline, ErrorCodes.InvalidNamespace);
})();
