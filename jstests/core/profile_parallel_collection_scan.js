// @tags: [does_not_support_stepdowns, requires_getmore]

// Confirms that a parallelCollectionScan command and subsequent getMores of its cursor are profiled
// correctly.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand.
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_parallel_collection_scan");
    var testColl = testDB.testColl;
    assert.commandWorked(testDB.dropDatabase());

    // Insert some data to scan over.
    assert.writeOK(testColl.insert([{}, {}, {}, {}]));

    testDB.setProfilingLevel(2);

    const parallelCollectionScanCmd = {parallelCollectionScan: testColl.getName(), numCursors: 1};
    const profileEntryFilter = {op: "command"};
    for (var field in parallelCollectionScanCmd) {
        profileEntryFilter['command.' + field] = parallelCollectionScanCmd[field];
    }

    let cmdRes = assert.commandWorked(testDB.runCommand(parallelCollectionScanCmd));

    assert.eq(testDB.system.profile.find(profileEntryFilter).itcount(),
              1,
              "expected to find profile entry for a parallelCollectionScan command");

    const firstCursor = cmdRes.cursors[0].cursor;
    const getMoreCollName = firstCursor.ns.substr(firstCursor.ns.indexOf(".") + 1);
    cmdRes = assert.commandWorked(
        testDB.runCommand({getMore: firstCursor.id, collection: getMoreCollName}));

    const getMoreProfileEntry = getLatestProfilerEntry(testDB, {op: "getmore"});
    for (var field in parallelCollectionScanCmd) {
        assert.eq(
            getMoreProfileEntry.originatingCommand[field], parallelCollectionScanCmd[field], field);
    }
})();
