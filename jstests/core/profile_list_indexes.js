// @tags: [
//   does_not_support_stepdowns,
//   requires_getmore,
//   requires_profiling,
//   requires_fcv_49,
// ]
// Requires fcv 4.9 for the changes to SimpleCursor

// Confirms that a listIndexes command and subsequent getMores of its cursor are profiled correctly.

(function() {
"use strict";

// For getLatestProfilerEntry and getProfilerProtocolStringForCommand.
load("jstests/libs/profiler.js");

var testDB = db.getSiblingDB("profile_list_indexes");
var testColl = testDB.testColl;
assert.commandWorked(testDB.dropDatabase());
const numIndexes = 5;
for (let i = 0; i < numIndexes; ++i) {
    let indexSpec = {};
    indexSpec["fakeField_" + i] = 1;
    assert.commandWorked(testColl.createIndex(indexSpec));
}

testDB.setProfilingLevel(2);

const listIndexesCommand = {
    listIndexes: testColl.getName(),
    cursor: {batchSize: NumberLong(1)}
};
const profileEntryFilter = {
    op: "command"
};
for (var field in listIndexesCommand) {
    profileEntryFilter['command.' + field] = listIndexesCommand[field];
}

let cmdRes = assert.commandWorked(testDB.runCommand(listIndexesCommand));

assert.eq(testDB.system.profile.find(profileEntryFilter).itcount(),
          1,
          "Expected to find profile entry for a listIndexes command");

const getMoreCollName = cmdRes.cursor.ns.substr(cmdRes.cursor.ns.indexOf(".") + 1);
cmdRes = assert.commandWorked(
    testDB.runCommand({getMore: cmdRes.cursor.id, collection: getMoreCollName}));

const getMoreProfileEntry = getLatestProfilerEntry(testDB, {op: "getmore"});
for (var field in listIndexesCommand) {
    assert.eq(getMoreProfileEntry.originatingCommand[field], listIndexesCommand[field], field);
}
})();
