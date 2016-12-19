// Confirms that profiled count execution contains all expected metrics with proper values.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_count");
    assert.commandWorked(testDB.dropDatabase());
    var conn = testDB.getMongo();
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Collection-level count.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    assert.eq(10, coll.count({}, {collation: {locale: "fr"}}));

    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.protocol, getProfilerProtocolStringForCommand(conn), tojson(profileObj));
    assert.eq(profileObj.command.count, coll.getName(), tojson(profileObj));
    assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
    assert.eq(profileObj.planSummary, "COUNT", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

    //
    // Count with non-indexed query.
    //
    coll.drop();
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    var query = {a: {$gte: 5}};
    assert.eq(5, coll.count(query));
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.command.query, query, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 10, tojson(profileObj));

    //
    // Count with indexed query.
    //
    coll.drop();
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    query = {a: {$gte: 5}};
    assert.eq(5, coll.count(query));
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.command.query, query, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 6, tojson(profileObj));
    assert.eq(profileObj.planSummary, "COUNT_SCAN { a: 1 }", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

    //
    // Confirm "fromMultiPlanner" metric.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    assert.eq(1, coll.count({a: 3, b: 3}));
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
})();
