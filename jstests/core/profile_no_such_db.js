// Test that reading the profiling level doesn't create databases, but setting it does.
(function(db) {
    'use strict';

    function dbExists() {
        return Array.contains(db.getMongo().getDBNames(), db.getName());
    }

    db = db.getSiblingDB('profile_no_such_db');  // Note: changes db argument not global var.
    assert.commandWorked(db.dropDatabase());
    assert(!dbExists());

    // Reading the profiling level shouldn't create the database.
    var defaultProfilingLevel = db.getProfilingLevel();
    assert(!dbExists());

    // This test assumes that the default profiling level hasn't been changed.
    assert.eq(defaultProfilingLevel, 0);

    [0, 1, 2].forEach(function(level) {
        jsTest.log('Testing profiling level ' + level);

        // Setting the profiling level creates the database.
        // Note: in storage engines other than MMAPv1 setting the profiling level to 0 puts the
        // database
        // in a weird state where it exists internally, but doesn't show up in listDatabases, and
        // won't
        // exist if you restart the server.
        var res = db.setProfilingLevel(level);
        assert.eq(res.was, defaultProfilingLevel);
        assert(dbExists() || level == 0);
        assert.eq(db.getProfilingLevel(), level);

        // Dropping the db reverts the profiling level to the default.
        assert.commandWorked(db.dropDatabase());
        assert.eq(db.getProfilingLevel(), defaultProfilingLevel);
        assert(!dbExists());
    });

}(db));
