// Checks that global histogram counters for collections are updated as we expect.

(function() {
    "use strict";
    var name = "operationalLatencyHistogramTest";

    var mongo = MongoRunner.runMongod({master: ""});
    var testDB = mongo.getDB("test");
    var testColl = testDB[name + "coll"];

    testColl.drop();

    function getHistogramStats() {
        return testDB.serverStatus({opLatencies: {histograms: 1}}).opLatencies;
    }

    var lastHistogram = getHistogramStats();

    // Checks that the difference in the histogram is what we expect, and also
    // accounts for the serverStatus command itself.
    function checkHistogramDiff(reads, writes, commands) {
        var thisHistogram = getHistogramStats();
        assert.eq(thisHistogram.reads.ops - lastHistogram.reads.ops, reads);
        assert.eq(thisHistogram.writes.ops - lastHistogram.writes.ops, writes);
        // Running the server status itself will increment command stats by one.
        assert.eq(thisHistogram.commands.ops - lastHistogram.commands.ops, commands + 1);
        return thisHistogram;
    }

    // Insert
    var numRecords = 100;
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.insert({_id: i}));
    }
    lastHistogram = checkHistogramDiff(0, numRecords, 0);

    // Update
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.update({_id: i}, {x: i}));
    }
    lastHistogram = checkHistogramDiff(0, numRecords, 0);

    // Find
    var cursors = [];
    for (var i = 0; i < numRecords; i++) {
        cursors[i] = testColl.find({x: {$gte: i}}).batchSize(2);
        assert.eq(cursors[i].next()._id, i);
    }
    lastHistogram = checkHistogramDiff(numRecords, 0, 0);

    // GetMore
    for (var i = 0; i < numRecords / 2; i++) {
        // Trigger two getmore commands.
        assert.eq(cursors[i].next()._id, i + 1);
        assert.eq(cursors[i].next()._id, i + 2);
        assert.eq(cursors[i].next()._id, i + 3);
        assert.eq(cursors[i].next()._id, i + 4);
    }
    lastHistogram = checkHistogramDiff(numRecords, 0, 0);

    // KillCursors
    // The last cursor has no additional results, hence does not need to be closed.
    for (var i = 0; i < numRecords - 1; i++) {
        cursors[i].close();
    }
    lastHistogram = checkHistogramDiff(0, 0, numRecords - 1);

    // Remove
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.remove({_id: i}));
    }
    lastHistogram = checkHistogramDiff(0, numRecords, 0);

    // Upsert
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.update({_id: i}, {x: i}, {upsert: 1}));
    }
    lastHistogram = checkHistogramDiff(0, numRecords, 0);

    // Aggregate
    for (var i = 0; i < numRecords; i++) {
        testColl.aggregate([{$match: {x: i}}, {$group: {_id: "$x"}}]);
    }
    lastHistogram = checkHistogramDiff(numRecords, 0, 0);

    // Count
    for (var i = 0; i < numRecords; i++) {
        testColl.count({x: i});
    }
    lastHistogram = checkHistogramDiff(numRecords, 0, 0);

    // Group
    testColl.group({initial: {}, reduce: function() {}, key: {a: 1}});
    lastHistogram = checkHistogramDiff(1, 0, 0);

    // ParallelCollectionScan
    testDB.runCommand({parallelCollectionScan: testColl.getName(), numCursors: 5});
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // FindAndModify
    testColl.findAndModify({query: {}, update: {pt: {type: "Point", coordinates: [0, 0]}}});
    lastHistogram = checkHistogramDiff(0, 1, 0);

    // CreateIndex
    assert.commandWorked(testColl.createIndex({pt: "2dsphere"}));
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // GeoNear
    assert.commandWorked(testDB.runCommand({
        geoNear: testColl.getName(),
        near: {type: "Point", coordinates: [0, 0]},
        spherical: true
    }));
    lastHistogram = checkHistogramDiff(1, 0, 0);

    // GetIndexes
    testColl.getIndexes();
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // Reindex
    assert.commandWorked(testColl.reIndex());
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // DropIndex
    assert.commandWorked(testColl.dropIndex({pt: "2dsphere"}));
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // Explain
    testColl.explain().find().next();
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // CollStats
    assert.commandWorked(testDB.runCommand({collStats: testColl.getName()}));
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // CollMod
    assert.commandWorked(
        testDB.runCommand({collStats: testColl.getName(), validationLevel: "off"}));
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // Compact
    var commandResult = testDB.runCommand({compact: testColl.getName()});
    // If storage engine supports compact, it should count as a command.
    if (!commandResult.ok) {
        assert.commandFailedWithCode(commandResult, ErrorCodes.CommandNotSupported);
    }
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // DataSize
    testColl.dataSize();
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // PlanCache
    testColl.getPlanCache().listQueryShapes();
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // ServerStatus
    assert.commandWorked(testDB.serverStatus());
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // WhatsMyURI
    assert.commandWorked(testColl.runCommand("whatsmyuri"));
    lastHistogram = checkHistogramDiff(0, 0, 1);

    // Test non-command.
    assert.commandFailed(testColl.runCommand("IHopeNobodyEverMakesThisACommand"));
    lastHistogram = checkHistogramDiff(0, 0, 1);
}());
