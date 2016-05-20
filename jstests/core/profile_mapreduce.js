// Confirms that profiled findAndModify execution contains all expected metrics with proper values.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_mapreduce");
    assert.commandWorked(testDB.dropDatabase());
    var conn = testDB.getMongo();
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    var mapFunction = function() {
        emit(this.a, this.b);
    };

    var reduceFunction = function(a, b) {
        return Array.sum(b);
    };

    //
    // Confirm metrics for mapReduce with query.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    coll.mapReduce(mapFunction, reduceFunction, {query: {a: {$gte: 0}}, out: {inline: 1}});

    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.keysExamined, 3, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { a: 1.0 }", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert.eq(profileObj.protocol, getProfilerProtocolStringForCommand(conn), tojson(profileObj));
    assert.eq(coll.getName(), profileObj.command.mapreduce, tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));

    //
    // Confirm metrics for mapReduce with sort stage.
    //
    coll.drop();
    for (var i = 0; i < 5; i++) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    coll.mapReduce(mapFunction, reduceFunction, {sort: {b: 1}, out: {inline: 1}});

    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.hasSortStage, true, tojson(profileObj));

    //
    // Confirm namespace field is correct when output is a collection.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    var outputCollectionName = "output_col";
    coll.mapReduce(mapFunction, reduceFunction, {query: {a: {$gte: 0}}, out: outputCollectionName});

    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));

    //
    // Confirm "fromMultiPlanner" metric.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    coll.mapReduce(mapFunction, reduceFunction, {query: {a: 3, b: 3}, out: {inline: 1}});
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
})();
