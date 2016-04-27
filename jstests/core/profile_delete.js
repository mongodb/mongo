// Confirms that profiled delete execution contains all expected metrics with proper values.

(function() {
    "use strict";

    load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

    // Setup test db and collection.
    var testDB = db.getSiblingDB("profile_delete");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Confirm metrics for single document delete.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    assert.writeOK(coll.remove({a: {$gte: 2}, b: {$gte: 2}}, {justOne: true}));

    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "remove", tojson(profileObj));
    assert.eq(profileObj.ndeleted, 1, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 1, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
    assert.eq(profileObj.keysDeleted, 2, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { a: 1.0 }", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));

    //
    // Confirm metrics for multiple document delete.
    //
    coll.drop();
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    assert.writeOK(coll.remove({a: {$gte: 2}}));
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ndeleted, 8, tojson(profileObj));
    assert.eq(profileObj.keysDeleted, 8, tojson(profileObj));

    //
    // Confirm "fromMultiPlanner" metric.
    //
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    for (i = 0; i < 5; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    assert.writeOK(coll.remove({a: 3, b: 3}));
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
})();
