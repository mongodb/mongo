(function() {
    "use strict";
    function profileCursor(query) {
        query = query || {};
        Object.extend(query, {user: username + "@" + db.getName()});
        return db.system.profile.find(query);
    }

    function getProfileAString() {
        var s = "\n";
        profileCursor().forEach(function(z) {
            s += tojson(z) + " ,\n";
        });
        return s;
    }

    function resetProfile(level, slowms) {
        db.setProfilingLevel(0);
        db.system.profile.drop();
        db.setProfilingLevel(level, slowms);
    }

    // special db so that it can be run in parallel tests
    var stddb = db;
    db = db.getSisterDB("profile1");
    var username = "jstests_profile1_user";

    db.dropUser(username);
    db.dropDatabase();

    try {
        db.createUser({user: username, pwd: "password", roles: jsTest.basicUserRoles});
        db.auth(username, "password");

        // With pre-created system.profile (capped)
        db.runCommand({profile: 0});
        db.getCollection("system.profile").drop();
        assert(!db.getLastError(), "Z");
        assert.eq(0, db.runCommand({profile: -1}).was, "A");

        // Create 32MB profile (capped) collection
        db.system.profile.drop();
        db.createCollection("system.profile", {capped: true, size: 32 * 1024 * 1024});
        db.runCommand({profile: 2});
        assert.eq(2, db.runCommand({profile: -1}).was, "B");
        assert.eq(1, db.system.profile.stats().capped, "C");

        db.foo.findOne();

        var profileItems = profileCursor().toArray();

        // create a msg for later if there is a failure.
        var msg = "";
        profileItems.forEach(function(d) {
            msg += "profile doc: " + d.ns + " " + d.op + " " +
                tojson(d.query ? d.query : d.command) + '\n';
        });
        msg += tojson(db.system.profile.stats());

        // If these nunmbers don't match, it is possible the collection has rolled over
        // (set to 32MB above in the hope this doesn't happen)
        assert.eq(2, profileItems.length, "E2 -- " + msg);

        // Make sure we can't drop if profiling is still on
        assert.throws(function(z) {
            db.getCollection("system.profile").drop();
        });

        // With pre-created system.profile (un-capped)
        db.runCommand({profile: 0});
        db.getCollection("system.profile").drop();
        assert.eq(0, db.runCommand({profile: -1}).was, "F");

        db.createCollection("system.profile");
        assert.eq(0, db.runCommand({profile: 2}).ok);
        assert.eq(0, db.runCommand({profile: -1}).was, "G");
        assert(!db.system.profile.stats().capped, "G1");

        // With no system.profile collection
        db.runCommand({profile: 0});
        db.getCollection("system.profile").drop();
        assert.eq(0, db.runCommand({profile: -1}).was, "H");

        db.runCommand({profile: 2});
        assert.eq(2, db.runCommand({profile: -1}).was, "I");
        assert.eq(1, db.system.profile.stats().capped, "J");

        resetProfile(2);
        db.profile1.drop();
        var q = {_id: 5};
        var u = {$inc: {x: 1}};
        db.profile1.update(q, u);
        var r = profileCursor({ns: db.profile1.getFullName()}).sort({$natural: -1})[0];
        assert.eq(q, r.query, "Y1: " + tojson(r));
        assert.eq(u, r.updateobj, "Y2");
        assert.eq("update", r.op, "Y3");
        assert.eq("profile1.profile1", r.ns, "Y4");
    } finally {
        // disable profiling for subsequent tests
        assert.commandWorked(db.runCommand({profile: 0}));
        db = stddb;
    }
}());
