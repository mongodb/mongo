// Basic tests for the $listLocalSessions {allUsers: true} aggregation stage.
//
// @tags: [
//   # This test attempts to start a session and find it using the $listLocalSessions stage. The
//   # former operation must be routed to the primary in a replica set, whereas the latter may be
//   # routed to a secondary.
//   assumes_read_preference_unchanged,
//   # The config fuzzer may run logical session cache refreshes in the background, which interferes
//   # with this test.
//   does_not_support_config_fuzzer,
//   # Sessions are asynchronously flushed to disk, so a stepdown immediately after calling
//   # startSession may cause this test to fail to find the returned sessionId.
//   does_not_support_stepdowns,
// ]

(function() {
'use strict';

const admin = db.getSiblingDB('admin');

// Get current log level.
let originalLogLevel = assert.commandWorked(admin.setLogLevel(1)).was.verbosity;

try {
    const listAllLocalSessions = function() {
        return admin.aggregate([{'$listLocalSessions': {allUsers: true}}]);
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
} finally {
    admin.setLogLevel(originalLogLevel);
}
})();
