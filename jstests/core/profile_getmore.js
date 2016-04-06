// Confirms that profiled getMore execution contains all expected metrics with proper values.
// TODO SERVER-23259: Add planSummary.
// TODO SERVER-23264: Add execStats.

(function() {
    "use strict";

    load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

    var testDB = db.getSiblingDB("profile_getmore");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Confirm basic metrics on getMore with a not-exhausted cursor.
    //
    var i;
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    var cursor = coll.find({a: {$gt: 0}}).batchSize(2);
    cursor.next();  // Perform initial query and consume first of 2 docs returned.

    var cursorId = getLatestProfilerEntry(testDB).cursorid;  // Save cursorid from find.

    cursor.next();  // Consume second of 2 docs from initial query.
    cursor.next();  // getMore performed, leaving open cursor.

    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "getmore", tojson(profileObj));
    assert.eq(profileObj.keysExamined, 2, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 2, tojson(profileObj));
    assert.eq(profileObj.cursorid, cursorId, tojson(profileObj));
    assert.eq(profileObj.nreturned, 2, tojson(profileObj));
    assert.eq(profileObj.query.batchSize, 2, tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Global"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Database"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Collection"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(!profileObj.hasOwnProperty("cursorExhausted"), tojson(profileObj));

    //
    // Confirm hasSortStage on getMore with a not-exhausted cursor and in-memory sort.
    //
    coll.drop();
    for (i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    cursor = coll.find({a: {$gt: 0}}).sort({a: 1}).batchSize(2);
    cursor.next();  // Perform initial query and consume first of 2 docs returned.
    cursor.next();  // Consume second of 2 docs from initial query.
    cursor.next();  // getMore performed, leaving open cursor.

    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.hasSortStage, true, tojson(profileObj));

    //
    // Confirm "cursorExhausted" metric.
    //
    coll.drop();
    for (i = 0; i < 3; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    cursor = coll.find().batchSize(2);
    cursor.next();     // Perform initial query and consume first of 3 docs returned.
    cursor.itcount();  // Exhaust the cursor.

    profileObj = getLatestProfilerEntry(testDB);

    assert(profileObj.hasOwnProperty("cursorid"),
           tojson(profileObj));  // cursorid should always be present on getMore.
    assert.neq(0, profileObj.cursorid, tojson(profileObj));
    assert.eq(profileObj.cursorExhausted, true, tojson(profileObj));
})();