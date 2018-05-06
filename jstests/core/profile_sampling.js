// Confirms that the number of profiled operations is consistent with the sampleRate, if set.
// @tags: [does_not_support_stepdowns, requires_fastcount]
(function() {
    "use strict";

    // Use a special db to support running other tests in parallel.
    const profileDB = db.getSisterDB("profile_sampling");
    const coll = profileDB.profile_sampling;

    profileDB.dropDatabase();

    let originalProfilingSettings;
    try {
        originalProfilingSettings = assert.commandWorked(profileDB.setProfilingLevel(0));
        profileDB.system.profile.drop();
        assert.eq(0, profileDB.system.profile.count());

        profileDB.createCollection(coll.getName());
        assert.writeOK(coll.insert({x: 1}));

        assert.commandWorked(profileDB.setProfilingLevel(1, {sampleRate: 0, slowms: -1}));

        assert.neq(null, coll.findOne({x: 1}));
        assert.eq(1, coll.find({x: 1}).count());
        assert.writeOK(coll.update({x: 1}, {$inc: {a: 1}}));

        assert.commandWorked(profileDB.setProfilingLevel(0));

        assert.eq(0, profileDB.system.profile.count());

        profileDB.system.profile.drop();
        assert.commandWorked(profileDB.setProfilingLevel(1, {sampleRate: 0.5, slowms: -1}));

        // This should generate about 500 profile log entries.
        for (let i = 0; i < 500; i++) {
            assert.neq(null, coll.findOne({x: 1}));
            assert.writeOK(coll.update({x: 1}, {$inc: {a: 1}}));
        }

        assert.commandWorked(profileDB.setProfilingLevel(0));

        assert.between(10, profileDB.system.profile.count(), 990);
        profileDB.system.profile.drop();

        // Profiling level of 2 should log all operations, regardless of sample rate setting.
        assert.commandWorked(profileDB.setProfilingLevel(2, {sampleRate: 0}));
        // This should generate exactly 1000 profile log entries.
        for (let i = 0; i < 5; i++) {
            assert.neq(null, coll.findOne({x: 1}));
            assert.writeOK(coll.update({x: 1}, {$inc: {a: 1}}));
        }
        assert.commandWorked(profileDB.setProfilingLevel(0));
        assert.eq(10, profileDB.system.profile.count());
        profileDB.system.profile.drop();

    } finally {
        let profileCmd = {};
        profileCmd.profile = originalProfilingSettings.was;
        profileCmd = Object.extend(profileCmd, originalProfilingSettings);
        delete profileCmd.was;
        delete profileCmd.ok;
        assert.commandWorked(profileDB.runCommand(profileCmd));
    }
}());
