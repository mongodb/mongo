// @tags: [requires_profiling]
(function() {
    "use strict";

    const conn = MerizoRunner.runMerizod();
    const uri = "merizodb://" + conn.host + "/test";
    const tests = [];

    // Asserts that system.profile contains only entries
    // with application.name = appname (or undefined)
    function assertProfileOnlyContainsAppName(db, appname) {
        const res = db.system.profile.distinct("appName");
        assert(res.length > 0, "system.profile does not contain any docs");
        if (res.length > 1 || res.indexOf(appname) === -1) {
            // Dump collection.
            print("dumping db.system.profile");
            db.system.profile.find().forEach((doc) => printjsononeline(doc));
            doassert(`system.profile expected to only have appName=${appname}` +
                     ` but found ${tojson(res)}`);
        }
    }

    tests.push(function testDefaultAppName() {
        const db = new Merizo(uri).getDB("test");
        assert.commandWorked(db.coll.insert({}));
        assertProfileOnlyContainsAppName(db, "MerizoDB Shell");
    });

    tests.push(function testAppName() {
        const db = new Merizo(uri + "?appName=TestAppName").getDB("test");
        assert.commandWorked(db.coll.insert({}));
        assertProfileOnlyContainsAppName(db, "TestAppName");
    });

    tests.push(function testMultiWordAppName() {
        const db = new Merizo(uri + "?appName=Test%20App%20Name").getDB("test");
        assert.commandWorked(db.coll.insert({}));
        assertProfileOnlyContainsAppName(db, "Test App Name");
    });

    tests.push(function testLongAppName() {
        // From MerizoDB Handshake specification:
        // The client.application.name cannot exceed 128 bytes. MerizoDB will return an error if
        // these limits are not adhered to, which will result in handshake failure. Drivers MUST
        // validate these values and truncate driver provided values if necessary.
        const longAppName = "a".repeat(129);
        assert.throws(() => new Merizo(uri + "?appName=" + longAppName));

        // But a 128 character appname should connect without issue.
        const notTooLongAppName = "a".repeat(128);
        const db = new Merizo(uri + "?appName=" + notTooLongAppName).getDB("test");
        assert.commandWorked(db.coll.insert({}));
        assertProfileOnlyContainsAppName(db, notTooLongAppName);
    });

    tests.push(function testLongAppNameWithMultiByteUTF8() {
        // Each epsilon character is two bytes in UTF-8.
        const longAppName = "\u0190".repeat(65);
        assert.throws(() => new Merizo(uri + "?appName=" + longAppName));

        // But a 128 character appname should connect without issue.
        const notTooLongAppName = "\u0190".repeat(64);
        const db = new Merizo(uri + "?appName=" + notTooLongAppName).getDB("test");
        assert.commandWorked(db.coll.insert({}));
        assertProfileOnlyContainsAppName(db, notTooLongAppName);
    });

    tests.forEach((test) => {
        const db = conn.getDB("test");
        db.dropDatabase();
        // Entries in db.system.profile have application name.
        db.setProfilingLevel(2);
        test();
    });

    MerizoRunner.stopMerizod(conn);
})();
