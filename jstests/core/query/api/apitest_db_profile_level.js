/**
 * Tests for setting of profile levels
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: getLog, setProfilingLevel.
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   requires_profiling,
 *   requires_fcv_62,
 * ]
 */

import {iterateMatchingLogLines} from "jstests/libs/log.js";

/*
 *  be sure the public collection API is complete
 */
assert(db.getProfilingLevel, "getProfilingLevel");
assert(db.setProfilingLevel, "setProfilingLevel");

// A test-specific database is used for profiler testing so as not to interfere with
// other tests that modify profiler level, when run in parallel.
const profileLevelDB = db.getSiblingDB("apitest_db_profile_level");

// Checks the log for the expected change in profile level and applicable database.
function profilerChangeWasLogged({from, to, db}) {
    const globalLog = assert.commandWorked(profileLevelDB.adminCommand({getLog: 'global'}));

    const fieldMatcher = {msg: "Profiler settings changed"};
    const lines = [...iterateMatchingLogLines(globalLog.log, fieldMatcher)];
    const matches = lines.filter((line) => {
        const attr = JSON.parse(line).attr;
        return attr.from.level == from && attr.to.level == to && attr.db == db;
    });
    return matches.length ? matches : false;
}

profileLevelDB.getProfilingLevel();
assert(!profilerChangeWasLogged({from: 0, to: -1, db: profileLevelDB}),
       "Didn't expect anything to be logged");

assert.throws(() => {
    profileLevelDB.setProfilingLevel(-1);
});

profileLevelDB.setProfilingLevel(0);
assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");
assert(profilerChangeWasLogged({from: 0, to: 0, db: profileLevelDB}),
       "Didn't find expected log line");

profileLevelDB.setProfilingLevel(1);
assert(profileLevelDB.getProfilingLevel() == 1, "p1");
assert(profilerChangeWasLogged({from: 0, to: 1, db: profileLevelDB}),
       "Didn't find expected log line");

profileLevelDB.setProfilingLevel(2);
assert(profileLevelDB.getProfilingLevel() == 2, "p2");
assert(profilerChangeWasLogged({from: 1, to: 2, db: profileLevelDB}),
       "Didn't find expected log line");

profileLevelDB.setProfilingLevel(0);
assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");
assert(profilerChangeWasLogged({from: 2, to: 0, db: profileLevelDB}),
       "Didn't find expected log line");

assert.throws(() => {
    profileLevelDB.setProfilingLevel(10);
});
// Check that didn't log an invalid profile level change.
assert(!profilerChangeWasLogged({from: 0, to: 10, db: profileLevelDB}), "Didn't expect log line");
