// @tags: [does_not_support_stepdowns, requires_getmore, requires_profiling]

// Confirms that profiled getMore execution contains all expected metrics with proper values.

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

    var cursor = coll.find({a: {$gt: 0}}).sort({a: 1}).batchSize(2);
    cursor.next();  // Perform initial query and consume first of 2 docs returned.

    var cursorId =
        getLatestProfilerEntry(testDB, {op: "query"}).cursorid;  // Save cursorid from find.

    cursor.next();  // Consume second of 2 docs from initial query.
    cursor.next();  // getMore performed, leaving open cursor.

    var profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "getmore", tojson(profileObj));
    assert.eq(profileObj.keysExamined, 2, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 2, tojson(profileObj));
    assert.eq(profileObj.cursorid, cursorId, tojson(profileObj));
    assert.eq(profileObj.nreturned, 2, tojson(profileObj));
    assert.eq(profileObj.command.getMore, cursorId, tojson(profileObj));
    assert.eq(profileObj.command.collection, coll.getName(), tojson(profileObj));
    assert.eq(profileObj.command.batchSize, 2, tojson(profileObj));
    assert.eq(profileObj.originatingCommand.filter, {a: {$gt: 0}});
    assert.eq(profileObj.originatingCommand.sort, {a: 1});
    assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Global"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Database"), tojson(profileObj));
    assert(profileObj.locks.hasOwnProperty("Collection"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
    assert(!profileObj.hasOwnProperty("cursorExhausted"), tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

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

    profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

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

    profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

    assert(profileObj.hasOwnProperty("cursorid"),
           tojson(profileObj));  // cursorid should always be present on getMore.
    assert.neq(0, profileObj.cursorid, tojson(profileObj));
    assert.eq(profileObj.cursorExhausted, true, tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

    //
    // Confirm getMore on aggregation.
    //
    coll.drop();
    for (i = 0; i < 20; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));

    var cursor = coll.aggregate([{$match: {a: {$gte: 0}}}], {cursor: {batchSize: 0}, hint: {a: 1}});
    var cursorId = getLatestProfilerEntry(testDB, {"command.aggregate": coll.getName()}).cursorid;
    assert.neq(0, cursorId);

    cursor.next();  // Consume the result set.

    profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});

    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "getmore", tojson(profileObj));
    assert.eq(profileObj.command.getMore, cursorId, tojson(profileObj));
    assert.eq(profileObj.command.collection, coll.getName(), tojson(profileObj));
    assert.eq(
        profileObj.originatingCommand.pipeline[0], {$match: {a: {$gte: 0}}}, tojson(profileObj));
    assert.eq(profileObj.cursorid, cursorId, tojson(profileObj));
    assert.eq(profileObj.nreturned, 20, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
    assert.eq(profileObj.cursorExhausted, true, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 20, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 20, tojson(profileObj));
    assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));
    assert.eq(profileObj.originatingCommand.hint, {a: 1}, tojson(profileObj));

    //
    // Confirm that originatingCommand is truncated in the profiler as { $truncated: <string>,
    // comment: <string> }
    //
    let docToInsert = {};

    for (i = 0; i < 501; i++) {
        docToInsert[i] = "a".repeat(150);
    }

    coll.drop();
    for (i = 0; i < 4; i++) {
        assert.writeOK(coll.insert(docToInsert));
    }

    cursor = coll.find(docToInsert).comment("profile_getmore").batchSize(2);
    assert.eq(cursor.itcount(), 4);  // Consume result set and trigger getMore.

    profileObj = getLatestProfilerEntry(testDB, {op: "getmore"});
    assert.eq((typeof profileObj.originatingCommand.$truncated), "string", tojson(profileObj));
    assert.eq(profileObj.originatingCommand.comment, "profile_getmore", tojson(profileObj));
})();
