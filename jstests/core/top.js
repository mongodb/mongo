/**
 * 1. check top numbers are correct
 */
(function() {
    load("jstests/libs/stats.js");

    var name = "toptest";

    var testDB = db.getSiblingDB(name);
    var testColl = testDB[name + "coll"];
    testColl.drop();

    // Perform an operation on the collection so that it is present in the "top" command's output.
    assert.eq(testColl.find({}).itcount(), 0);

    //  This variable is used to get differential output
    var lastTop = getTop(testColl);

    var numRecords = 100;

    //  Insert
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.insert({_id: i}));
    }
    assertTopDiffEq(testColl, lastTop, "insert", numRecords);
    lastTop = assertTopDiffEq(testColl, lastTop, "writeLock", numRecords);

    // Update
    for (i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.update({_id: i}, {x: i}));
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "update", numRecords);

    // Queries
    var query = {};
    for (i = 0; i < numRecords; i++) {
        query[i] = testColl.find({x: {$gte: i}}).batchSize(2);
        assert.eq(query[i].next()._id, i);
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "queries", numRecords);

    // Getmore
    for (i = 0; i < numRecords / 2; i++) {
        assert.eq(query[i].next()._id, i + 1);
        assert.eq(query[i].next()._id, i + 2);
        assert.eq(query[i].next()._id, i + 3);
        assert.eq(query[i].next()._id, i + 4);
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "getmore", numRecords);

    // Remove
    for (i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.remove({_id: 1}));
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "remove", numRecords);

    // Upsert, note that these are counted as updates, not inserts
    for (i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.update({_id: i}, {x: i}, {upsert: 1}));
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "update", numRecords);

    // Commands
    var res;

    // "count" command
    lastTop = getTop(testColl);  // ignore any commands before this
    for (i = 0; i < numRecords; i++) {
        res = assert.commandWorked(testDB.runCommand({count: testColl.getName()}));
        assert.eq(res.n, numRecords, tojson(res));
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "commands", numRecords);

    // "findAndModify" command
    lastTop = getTop(testColl);
    for (i = 0; i < numRecords; i++) {
        res = assert.commandWorked(testDB.runCommand({
            findAndModify: testColl.getName(),
            query: {_id: i},
            update: {$inc: {x: 1}},
        }));
        assert.eq(res.value.x, i, tojson(res));
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "commands", numRecords);

    lastTop = getTop(testColl);
    for (i = 0; i < numRecords; i++) {
        res = assert.commandWorked(testDB.runCommand({
            findAndModify: testColl.getName(),
            query: {_id: i},
            remove: true,
        }));
        assert.eq(res.value.x, i + 1, tojson(res));
    }
    lastTop = assertTopDiffEq(testColl, lastTop, "commands", numRecords);
}());
