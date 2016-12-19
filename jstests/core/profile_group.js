// Confirms that profiled group execution contains all expected metrics with proper values.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_group");
    assert.commandWorked(testDB.dropDatabase());
    var conn = testDB.getMongo();
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Confirm standard group command metrics.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i, b: i % 5}));
    }
    assert.commandWorked(coll.createIndex({b: -1}));

    coll.group({
        key: {a: 1, b: 1},
        cond: {b: 3},
        reduce: function() {},
        initial: {},
        collation: {locale: "fr"}
    });
    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.keysExamined, 2, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 2, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { b: -1 }", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert.eq(profileObj.protocol, getProfilerProtocolStringForCommand(conn), tojson(profileObj));
    assert.eq(profileObj.command.group.key, {a: 1, b: 1}, tojson(profileObj));
    assert.eq(profileObj.command.group.collation, {locale: "fr"}, tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.command.hasOwnProperty("group"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
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

    coll.group({key: {a: 1, b: 1}, cond: {a: 3, b: 3}, reduce: function() {}, initial: {}});
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
})();
