// Basic tests for the $listSessions aggregation stage.

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
    var resultArray;
    assert.soon(function() {
        resultArray = listSessions().toArray();
        if (resultArray.length < 1) {
            return false;
        }
        const resultArrayMine = resultArray
                                    .map(function(sess) {
                                        return sess._id.id;
                                    })
                                    .filter(function(id) {
                                        return 0 == bsonWoCompare({x: id}, {x: myid});
                                    });
        return resultArrayMine.length == 1;
    }, "Failed to locate session in collection");

    // Try asking for the session by username.
    const myusername = (function() {
        if (0 == bsonWoCompare({x: resultArray[0]._id.uid}, {x: computeSHA256Block("")})) {
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
    const myArray = listMySessions().toArray();
    assert.eq(resultArray.length, myArray.length);

    // Make sure pipelining other collections fail.
    assertErrorCode(admin.system.collections, pipeline, ErrorCodes.InvalidNamespace);
})();
