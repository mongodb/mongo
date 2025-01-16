// Confirms that a listCollections command is not profiled.
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

var testDB = db.getSiblingDB("profile_list_collections");
assert.commandWorked(testDB.dropDatabase());
const numCollections = 5;
for (let i = 0; i < numCollections; ++i) {
    assert.commandWorked(testDB.runCommand({create: "test_" + i}));
}

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDB.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

const profileEntryFilter = {
    op: "command",
    command: "listCollections"
};

let cmdRes = assert.commandWorked(testDB.runCommand({listCollections: 1, cursor: {batchSize: 1}}));

// We don't profile listCollections commands.
assert.eq(testDB.system.profile.find(profileEntryFilter).itcount(),
          0,
          "Did not expect any profile entry for a listCollections command");

const getMoreCollName = cmdRes.cursor.ns.substr(cmdRes.cursor.ns.indexOf(".") + 1);
cmdRes = assert.commandWorked(
    testDB.runCommand({getMore: cmdRes.cursor.id, collection: getMoreCollName}));

// We disabled profiling getMore commands that originate from a listCollection command, given that
// the original command was not profiled either.
assert.throws(() => getLatestProfilerEntry(testDB, {op: "getmore"}),
              [],
              "Did not expect to find entry for getMore on a listCollections cursor");
