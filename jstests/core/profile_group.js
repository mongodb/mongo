// Confirms that profiled group execution contains all expected metrics with proper values.
// TODO SERVER-23257: Add keysExamined, docsExamined.
// TODO SERVER-23259: Add planSummary.
// TODO SERVER-23264: Add execStats.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand
    load("jstests/libs/profiler.js");

    var conn = new Mongo(db.getMongo().host);
    var testDB = conn.getDB("profile_group");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Confirm standard group command metrics.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i, b: i % 5}));
    }

    coll.group({key: {a: 1, b: 1}, cond: {b: 3}, reduce: function() {}, initial: {}});
    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.protocol, getProfilerProtocolStringForCommand(conn), tojson(profileObj));
    assert.eq(profileObj.command.group.key, {a: 1, b: 1}, tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.command.hasOwnProperty("group"), tojson(profileObj));
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

    coll.group({key: {a: 1, b: 1}, cond: {a: 3, b: 3}, reduce: function() {}, initial: {}});
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
})();