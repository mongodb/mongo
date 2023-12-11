// Test that reading the profiling level doesn't create databases, but setting it does.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_profiling
// ]

function dbExists(db) {
    return Array.contains(db.getMongo().getDBNames(), db.getName());
}

const testDb = db.getSiblingDB('profile_no_such_db');  // Note: changes db argument not global var.
assert.commandWorked(testDb.dropDatabase());
assert(!dbExists(testDb));

// Reading the profiling level shouldn't create the database.
var defaultProfilingLevel = testDb.getProfilingLevel();
assert(!dbExists(testDb));

// This test assumes that the default profiling level hasn't been changed.
assert.eq(defaultProfilingLevel, 0);

[0, 1, 2].forEach(function(level) {
    jsTest.log('Testing profiling level ' + level);

    // Setting the profiling level creates the database.
    // Note: setting the profiling level to 0 puts the database in a weird state where it
    // exists internally, but doesn't show up in listDatabases, and won't exist if you
    // restart the server.
    var res = testDb.setProfilingLevel(level);
    assert.eq(res.was, defaultProfilingLevel);
    assert(dbExists(testDb) || level == 0);
    assert.eq(testDb.getProfilingLevel(), level);

    // Dropping the db reverts the profiling level to the default.
    assert.commandWorked(testDb.dropDatabase());
    assert.eq(testDb.getProfilingLevel(), defaultProfilingLevel);
    assert(!dbExists(testDb));
});
