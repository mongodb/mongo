// Confirms that profiled distinct execution contains all expected metrics with proper values.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_distinct");
    assert.commandWorked(testDB.dropDatabase());
    var conn = testDB.getMongo();
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Confirm metrics for distinct with query.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i % 5, b: i}));
    }
    assert.commandWorked(coll.createIndex({b: 1}));

    coll.distinct("a", {b: {$gte: 5}});
    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.keysExamined, 5, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 5, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { b: 1.0 }", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert.eq(profileObj.protocol, getProfilerProtocolStringForCommand(conn), tojson(profileObj));
    assert.eq(coll.getName(), profileObj.command.distinct, tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));

    //
    // Confirm "fromMultiPlanner" metric.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    coll.distinct("a", {a: 3, b: 3});
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
})();
