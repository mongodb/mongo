// Basic tests for the $listLocalSessions {allUsers: true} aggregation stage.

(function() {
    'use strict';

    const listAllLocalSessions = function() {
        return db.aggregate([{'$listLocalSessions': {allUsers: true}}]);
    };

    // Start a new session and capture its sessionId.
    const myid = assert.commandWorked(db.runCommand({startSession: 1})).id.id;
    assert(myid !== undefined);

    // Ensure that the cache now contains the session and is visible by admin.
    const resultArray = assert.doesNotThrow(listAllLocalSessions).toArray();
    assert.gte(resultArray.length, 1);
    const resultArrayMine = resultArray
                                .map(function(sess) {
                                    return sess._id.id;
                                })
                                .filter(function(id) {
                                    return 0 == bsonWoCompare({x: id}, {x: myid});
                                });
    assert.eq(resultArrayMine.length, 1);
})();
