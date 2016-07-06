// Confirms that the number of profiled operations is consistent with the sampleRate, if set.

(function () {
    "use strict";

    // Use a special db to support running other tests in parallel.
    var profileDB = db.getSisterDB("profile_sampling");
    var username = "jstests_profile_sampling_user";

    var coll = profileDB.profile_sampling;

    profileDB.dropDatabase();

    var origprof = 0;
    try {
        origprof = profileDB.setProfilingLevel(0).was;
        profileDB.system.profile.drop();
        assert.eq(0, profileDB.system.profile.count());

        profileDB.createCollection(coll.getName());
        assert.writeOK(coll.insert({x: 1}));

        assert.commandWorked(profileDB.setProfilingLevel(2, {sampleRate: 0}));

        assert.neq(null, coll.findOne({x: 1}));
        assert.eq(1, coll.find({x: 1}).count());
        assert.writeOK(coll.update({x: 1}, {$inc: {a: 1}}));

        profileDB.setProfilingLevel(0);

        profileDB.system.profile.find().forEach(printjson);
        assert.eq(0, profileDB.system.profile.count());

        profileDB.system.profile.drop();
        assert.commandWorked(profileDB.setProfilingLevel(2, {sampleRate: 0.5}));

        // This should generate about 500 profile log entries.
        for (var i = 0; i < 500; i++) {
            assert.neq(null, coll.findOne({x: 1}));
            assert.writeOK(coll.update({x: 1}, {$inc: {a: 1}}));
        }

        profileDB.setProfilingLevel(0);

        assert.between(10, profileDB.system.profile.count(), 990);
    } finally {
        profileDB.setProfilingLevel(origprof);
    }
}());
