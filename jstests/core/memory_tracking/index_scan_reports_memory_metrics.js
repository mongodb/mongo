/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for queries that
 * use record deduplication during index scan.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   requires_fcv_82,
 * ]
 */

import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const stageName = "IXSCAN";
const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

for (let i = 0; i < 10; ++i) {
    //'a' is an array to create a multikey index.
    assert.commandWorked(coll.insertOne({_id: i, a: [1, 2, 3, 4, 5]}));
}

assert.commandWorked(coll.createIndex({a: 1}));

let pipeline = [{$match: {a: 5}}];

runMemoryStatsTest({
    db,
    collName,
    commandObj: {
        aggregate: collName,
        pipeline,
        comment: "memory stats index scan stage test",
        allowDiskUse: false,
        cursor: {batchSize: 1}
    },
    stageName,
    expectedNumGetMores: 10,
    // This stage does not release memory on EOF.
    checkInUseMemBytesResets: false,
});

// Clean up.
db[collName].drop();
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
