// Basic tests for the $listSessions {allUsers:true} aggregation stage.

(function() {
    'use strict';
    load('jstests/aggregation/extras/utils.js');

    const admin = db.getSiblingDB("admin");
    const pipeline = [{'$listSessions': {allUsers: true}}];
    function listSessions() {
        return admin.system.sessions.aggregate(pipeline);
    }

    // Start a new session and capture its sessionId.
    const myid = assert.commandWorked(admin.runCommand({startSession: 1})).id.id;
    assert(myid !== undefined);
    assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));

    // Ensure that the cache now contains the session and is visible by admin.
    assert.soon(function() {
        const resultArray = listSessions().toArray();
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

    // Make sure pipelining other collections fail.
    assertErrorCode(admin.system.collections, pipeline, ErrorCodes.InvalidNamespace);
})();
