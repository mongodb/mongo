// Confirms that profiled aggregation execution contains all expected metrics with proper values.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_agg");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Confirm metrics for agg w/ $match.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    assert.eq(8, coll.aggregate([{$match: {a: {$gte: 2}}}], {collation: {locale: "fr"}}).itcount());
    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.nreturned, 8, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 8, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 8, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
    assert.eq(profileObj.protocol,
              getProfilerProtocolStringForCommand(testDB.getMongo()),
              tojson(profileObj));
    assert.eq(profileObj.command.aggregate, coll.getName(), tojson(profileObj));
    assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
    assert(!profileObj.hasOwnProperty("hasSortStage"), tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

    //
    // Confirm hasSortStage with in-memory sort.
    //
    coll.drop();
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    assert.eq(8, coll.aggregate([{$match: {a: {$gte: 2}}}, {$sort: {a: 1}}]).itcount());
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.hasSortStage, true, tojson(profileObj));

    //
    // Confirm "fromMultiPlanner" metric.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    assert.eq(1, coll.aggregate([{$match: {a: 3, b: 3}}]).itcount());
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
})();
