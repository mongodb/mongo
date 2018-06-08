/**
 *  Tests for setting of profile levels
 *  @tags: [does_not_support_stepdowns, requires_profiling]
 */

(function() {
    'use strict';

    /*
     *  be sure the public collection API is complete
     */
    assert(db.getProfilingLevel, "getProfilingLevel");
    assert(db.setProfilingLevel, "setProfilingLevel");

    // A test-specific database is used for profiler testing so as not to interfere with
    // other tests that modify profiler level, when run in parallel.
    var profileLevelDB = db.getSiblingDB("apitest_db_profile_level");

    profileLevelDB.setProfilingLevel(0);
    assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");

    profileLevelDB.setProfilingLevel(1);
    assert(profileLevelDB.getProfilingLevel() == 1, "p1");

    profileLevelDB.setProfilingLevel(2);
    assert(profileLevelDB.getProfilingLevel() == 2, "p2");

    profileLevelDB.setProfilingLevel(0);
    assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");

    var asserted = false;
    try {
        profileLevelDB.setProfilingLevel(10);
        assert(false);
    } catch (e) {
        asserted = true;
        assert(e.dbSetProfilingException);
    }
    assert(asserted, "should have asserted");
})();
