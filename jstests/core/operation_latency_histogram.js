// Checks that histogram counters for collections are updated as we expect.

(function() {
    "use strict";

    load("jstests/libs/stats.js");
    var name = "operationalLatencyHistogramTest";

    var testDB = db.getSiblingDB(name);
    var testColl = testDB[name + "coll"];

    testColl.drop();

    // Test aggregation command output format.
    var commandResult = testDB.runCommand(
        {aggregate: testColl.getName(), pipeline: [{$collStats: {latencyStats: {}}}]});
    assert.commandWorked(commandResult);
    assert(commandResult.result.length == 1);

    var stats = commandResult.result[0];
    var histogramTypes = ["reads", "writes", "commands"];

    assert(stats.hasOwnProperty("localTime"));
    assert(stats.hasOwnProperty("latencyStats"));

    histogramTypes.forEach(function(key) {
        assert(stats.latencyStats.hasOwnProperty(key));
        assert(stats.latencyStats[key].hasOwnProperty("ops"));
        assert(stats.latencyStats[key].hasOwnProperty("latency"));
    });

    var lastHistogram = getHistogramStats(testColl);

    // Insert
    var numRecords = 100;
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.insert({_id: i}));
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, numRecords, 0);

    // Update
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.update({_id: i}, {x: i}));
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, numRecords, 0);

    // Find
    var cursors = [];
    for (var i = 0; i < numRecords; i++) {
        cursors[i] = testColl.find({x: {$gte: i}}).batchSize(2);
        assert.eq(cursors[i].next()._id, i);
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, numRecords, 0, 0);

    // GetMore
    for (var i = 0; i < numRecords / 2; i++) {
        // Trigger two getmore commands.
        assert.eq(cursors[i].next()._id, i + 1);
        assert.eq(cursors[i].next()._id, i + 2);
        assert.eq(cursors[i].next()._id, i + 3);
        assert.eq(cursors[i].next()._id, i + 4);
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, numRecords, 0, 0);

    // KillCursors
    // The last cursor has no additional results, hence does not need to be closed.
    for (var i = 0; i < numRecords - 1; i++) {
        cursors[i].close();
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, numRecords - 1);

    // Remove
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.remove({_id: i}));
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, numRecords, 0);

    // Upsert
    for (var i = 0; i < numRecords; i++) {
        assert.writeOK(testColl.update({_id: i}, {x: i}, {upsert: 1}));
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, numRecords, 0);

    // Aggregate
    for (var i = 0; i < numRecords; i++) {
        testColl.aggregate([{$match: {x: i}}, {$group: {_id: "$x"}}]);
    }
    // TODO SERVER-24704: Agg is currently counted by Top as two operations, but should be counted
    // as one.
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 2 * numRecords, 0, 0);

    // Count
    for (var i = 0; i < numRecords; i++) {
        testColl.count({x: i});
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, numRecords, 0, 0);

    // Group
    testColl.group({initial: {}, reduce: function() {}, key: {a: 1}});
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 1, 0, 0);

    // ParallelCollectionScan
    testDB.runCommand({parallelCollectionScan: testColl.getName(), numCursors: 5});
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // FindAndModify
    testColl.findAndModify({query: {}, update: {pt: {type: "Point", coordinates: [0, 0]}}});
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 1, 0);

    // CreateIndex
    assert.commandWorked(testColl.createIndex({pt: "2dsphere"}));
    // TODO SERVER-24705: createIndex is not currently counted in Top.
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 0);

    // GeoNear
    assert.commandWorked(testDB.runCommand({
        geoNear: testColl.getName(),
        near: {type: "Point", coordinates: [0, 0]},
        spherical: true
    }));
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 1, 0, 0);

    // GetIndexes
    testColl.getIndexes();
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // Reindex
    assert.commandWorked(testColl.reIndex());
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // DropIndex
    assert.commandWorked(testColl.dropIndex({pt: "2dsphere"}));
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // Explain
    testColl.explain().find().next();
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // CollStats
    assert.commandWorked(testDB.runCommand({collStats: testColl.getName()}));
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // CollMod
    assert.commandWorked(
        testDB.runCommand({collStats: testColl.getName(), validationLevel: "off"}));
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // Compact
    // Use force:true in case we're in replset.
    var commandResult = testDB.runCommand({compact: testColl.getName(), force: true});
    // If storage engine supports compact, it should count as a command.
    if (!commandResult.ok) {
        assert.commandFailedWithCode(commandResult, ErrorCodes.CommandNotSupported);
    }
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // DataSize
    testColl.dataSize();
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // PlanCache
    testColl.getPlanCache().listQueryShapes();
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 1);

    // Commands which occur on the database only should not effect the collection stats.
    assert.commandWorked(testDB.serverStatus());
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 0);

    assert.commandWorked(testColl.runCommand("whatsmyuri"));
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 0);

    // Test non-command.
    assert.commandFailed(testColl.runCommand("IHopeNobodyEverMakesThisACommand"));
    lastHistogram = assertHistogramDiffEq(testColl, lastHistogram, 0, 0, 0);
}());