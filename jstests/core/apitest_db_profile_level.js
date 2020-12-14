/**
 *  Tests for setting of profile levels
 *  @tags: [does_not_support_stepdowns, requires_profiling, requires_fcv_40]
 */

(function() {
    'use strict';

    load("jstests/libs/log.js");  // For findMatchingLogLine, findMatchingLogLines.

    /*
     *  be sure the public collection API is complete
     */
    assert(db.getProfilingLevel, "getProfilingLevel");
    assert(db.setProfilingLevel, "setProfilingLevel");

    // A test-specific database is used for profiler testing so as not to interfere with
    // other tests that modify profiler level, when run in parallel.
    var profileLevelDB = db.getSiblingDB("apitest_db_profile_level");

    // Checks for the log that was expected to be created when profile level changed.
    function profilerChangeWasLogged({from, to} = {}) {
        const globalLog = assert.commandWorked(profileLevelDB.adminCommand({getLog: 'global'}));

        const fieldMatcher = {msg: "Profiler settings changed"};
        if (from && to) {
            const lines = [...findMatchingLogLines(globalLog.log, fieldMatcher)];
            return lines.find(line => line.match(new RegExp(/from:\{ /.source + from.source)) &&
                                  line.match(new RegExp(/to:\{ /.source + to.source)));
        } else {
            return findMatchingLogLine(globalLog.log, fieldMatcher);
        }
    }

    profileLevelDB.getProfilingLevel();
    assert(!profilerChangeWasLogged({from: /level: 0/, to: /level: -1/}),
           "Didn't expect anything to be logged");

    assert.throws(() => {
        profileLevelDB.setProfilingLevel(-1);
    });

    profileLevelDB.setProfilingLevel(0);
    assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");
    assert(profilerChangeWasLogged({from: /level: 0/, to: /level: 0/}),
           "Didn't find expected log line");

    profileLevelDB.setProfilingLevel(1);
    assert(profileLevelDB.getProfilingLevel() == 1, "p1");
    assert(profilerChangeWasLogged({from: /level: 0/, to: /level: 1/}),
           "Didn't find expected log line");

    profileLevelDB.setProfilingLevel(2);
    assert(profileLevelDB.getProfilingLevel() == 2, "p2");
    assert(profilerChangeWasLogged({from: /level: 1/, to: /level: 2/}),
           "Didn't find expected log line");

    profileLevelDB.setProfilingLevel(0);
    assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");
    assert(profilerChangeWasLogged({from: /level: 2/, to: /level: 0/}),
           "Didn't find expected log line");

    assert.throws(() => {
        profileLevelDB.setProfilingLevel(10);
    });
    // Check that didn't log an invalid profile level change.
    assert(!profilerChangeWasLogged({from: /level: 0/, to: /level: 10/}), "Didn't expect log line");
})();
