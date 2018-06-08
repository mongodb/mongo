// @tags: [does_not_support_stepdowns, requires_getmore, requires_profiling]

// Confirms that a listCollections command is not profiled.

(function() {
    "use strict";

    // For getLatestProfilerEntry and getProfilerProtocolStringForCommand.
    load("jstests/libs/profiler.js");

    var testDB = db.getSiblingDB("profile_list_collections");
    assert.commandWorked(testDB.dropDatabase());
    const numCollections = 5;
    for (let i = 0; i < numCollections; ++i) {
        assert.commandWorked(testDB.runCommand({create: "test_" + i}));
    }

    testDB.setProfilingLevel(2);

    const profileEntryFilter = {op: "command", command: "listCollections"};

    let cmdRes =
        assert.commandWorked(testDB.runCommand({listCollections: 1, cursor: {batchSize: 1}}));

    // We don't profile listCollections commands.
    assert.eq(testDB.system.profile.find(profileEntryFilter).itcount(),
              0,
              "Did not expect any profile entry for a listCollections command");

    const getMoreCollName = cmdRes.cursor.ns.substr(cmdRes.cursor.ns.indexOf(".") + 1);
    cmdRes = assert.commandWorked(
        testDB.runCommand({getMore: cmdRes.cursor.id, collection: getMoreCollName}));

    // A listCollections cursor doesn't really have a namespace to use to record profile entries, so
    // does not get recorded in the profile.
    assert.throws(() => getLatestProfilerEntry(testDB, {op: "getmore"}),
                  [],
                  "Did not expect to find entry for getMore on a listCollections cursor");
})();
