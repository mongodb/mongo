// Confirms that the number of profiled operations is consistent with the sampleRate, if set.
(function() {
    "use strict";

    // Use a special db to support running other tests in parallel.
    var profileDB = db.getSisterDB("profile_sampling");
    var coll = profileDB.profile_sampling;

    profileDB.dropDatabase();

    var originalProfilingSettings;
    try {
        originalProfilingSettings = assert.commandWorked(profileDB.setProfilingLevel(0));
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
        let profileCmd = {};
        profileCmd.profile = originalProfilingSettings.was;
        profileCmd = Object.extend(profileCmd, originalProfilingSettings);
        delete profileCmd.was;
        delete profileCmd.ok;
        assert.commandWorked(profileDB.runCommand(profileCmd));
    }
}());
