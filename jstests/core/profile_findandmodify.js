// Confirms that profiled findAndModify execution contains all expected metrics with proper values.

(function() {
    "use strict";

    load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

    var testDB = db.getSiblingDB("profile_findandmodify");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");

    testDB.setProfilingLevel(2);

    //
    // Update as findAndModify.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({b: 1}));

    assert.eq({_id: 2, a: 2, b: 2}, coll.findAndModify({query: {a: 2}, update: {$inc: {b: 1}}}));

    var profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
    assert.eq(profileObj.command.update, {$inc: {b: 1}}, tojson(profileObj));
    assert.eq(profileObj.updateobj, {$inc: {b: 1}}, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
    assert.eq(profileObj.nMatched, 1, tojson(profileObj));
    assert.eq(profileObj.nModified, 1, tojson(profileObj));
    assert.eq(profileObj.keysInserted, 1, tojson(profileObj));
    assert.eq(profileObj.keysDeleted, 1, tojson(profileObj));
    assert.eq(profileObj.planSummary, "COLLSCAN", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
    assert(profileObj.hasOwnProperty("responseLength"), tojson(profileObj));

    //
    // Delete as findAndModify.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    assert.eq({_id: 2, a: 2}, coll.findAndModify({query: {a: 2}, remove: true}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
    assert.eq(profileObj.command.remove, true, tojson(profileObj));
    assert(!profileObj.hasOwnProperty("updateobj"), tojson(profileObj));
    assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
    assert.eq(profileObj.ndeleted, 1, tojson(profileObj));
    assert.eq(profileObj.keysDeleted, 1, tojson(profileObj));
    assert.eq(profileObj.planSummary, "COLLSCAN", tojson(profileObj));
    assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));

    //
    // Update with {upsert: true} as findAndModify.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    assert.eq(
        {_id: 4, a: 1},
        coll.findAndModify({query: {_id: 4}, update: {$inc: {a: 1}}, upsert: true, new: true}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.query, {_id: 4}, tojson(profileObj));
    assert.eq(profileObj.command.update, {$inc: {a: 1}}, tojson(profileObj));
    assert.eq(profileObj.command.upsert, true, tojson(profileObj));
    assert.eq(profileObj.command.new, true, tojson(profileObj));
    assert.eq(profileObj.updateobj, {$inc: {a: 1}}, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 0, tojson(profileObj));
    assert.eq(profileObj.nMatched, 0, tojson(profileObj));
    assert.eq(profileObj.nModified, 0, tojson(profileObj));
    assert.eq(profileObj.upsert, true, tojson(profileObj));
    assert.eq(profileObj.keysInserted, 1, tojson(profileObj));

    //
    // Idhack update as findAndModify.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    assert.eq({_id: 2, a: 2}, coll.findAndModify({query: {_id: 2}, update: {$inc: {b: 1}}}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.keysExamined, 1, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
    assert.eq(profileObj.nMatched, 1, tojson(profileObj));
    assert.eq(profileObj.nModified, 1, tojson(profileObj));
    assert.eq(profileObj.planSummary, "IDHACK", tojson(profileObj));

    //
    // Update as findAndModify with projection.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    assert.eq({a: 2},
              coll.findAndModify({query: {a: 2}, update: {$inc: {b: 1}}, fields: {_id: 0, a: 1}}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
    assert.eq(profileObj.command.update, {$inc: {b: 1}}, tojson(profileObj));
    assert.eq(profileObj.command.fields, {_id: 0, a: 1}, tojson(profileObj));
    assert.eq(profileObj.updateobj, {$inc: {b: 1}}, tojson(profileObj));
    assert.eq(profileObj.keysExamined, 0, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 3, tojson(profileObj));
    assert.eq(profileObj.nMatched, 1, tojson(profileObj));
    assert.eq(profileObj.nModified, 1, tojson(profileObj));

    //
    // Delete as findAndModify with projection.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    assert.eq({a: 2}, coll.findAndModify({query: {a: 2}, remove: true, fields: {_id: 0, a: 1}}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.query, {a: 2}, tojson(profileObj));
    assert.eq(profileObj.command.remove, true, tojson(profileObj));
    assert.eq(profileObj.command.fields, {_id: 0, a: 1}, tojson(profileObj));
    assert(!profileObj.hasOwnProperty("updateobj"), tojson(profileObj));
    assert.eq(profileObj.ndeleted, 1, tojson(profileObj));

    //
    // Confirm "hasSortStage" on findAndModify with sort.
    //
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    assert.eq({_id: 0, a: 0},
              coll.findAndModify({query: {a: {$gte: 0}}, sort: {a: 1}, update: {$inc: {b: 1}}}));

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

    coll.findAndModify({query: {a: 3, b: 3}, update: {$set: {c: 1}}});
    profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
})();