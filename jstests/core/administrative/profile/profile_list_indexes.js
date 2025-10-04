// Confirms that a listIndexes command and subsequent getMores of its cursor are profiled correctly.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_getmore,
//   requires_profiling,
//   # The test queries the system.profile collection so it is not compatible with initial sync
//   # since an initial sync may insert unexpected operations into the profile collection.
//   queries_system_profile_collection
// ]

import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

let testDB = db.getSiblingDB("profile_list_indexes");
let testColl = testDB.testColl;
assert.commandWorked(testDB.dropDatabase());
const numIndexes = 5;
for (let i = 0; i < numIndexes; ++i) {
    let indexSpec = {};
    indexSpec["fakeField_" + i] = 1;
    assert.commandWorked(testColl.createIndex(indexSpec));
}

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(
    testDB.setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

const listIndexesCommand = {
    listIndexes: testColl.getName(),
    cursor: {batchSize: NumberLong(1)},
};
const profileEntryFilter = {
    op: "command",
};
for (var field in listIndexesCommand) {
    profileEntryFilter["command." + field] = listIndexesCommand[field];
}

let cmdRes = assert.commandWorked(testDB.runCommand(listIndexesCommand));

assert.eq(
    testDB.system.profile.find(profileEntryFilter).itcount(),
    1,
    "Expected to find profile entry for a listIndexes command",
);

const getMoreCollName = cmdRes.cursor.ns.substr(cmdRes.cursor.ns.indexOf(".") + 1);
cmdRes = assert.commandWorked(testDB.runCommand({getMore: cmdRes.cursor.id, collection: getMoreCollName}));

const getMoreProfileEntry = getLatestProfilerEntry(testDB, {op: "getmore"});
for (var field in listIndexesCommand) {
    assert.eq(getMoreProfileEntry.originatingCommand[field], listIndexesCommand[field], field);
}
