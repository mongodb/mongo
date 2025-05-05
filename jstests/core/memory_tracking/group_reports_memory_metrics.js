/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $group using the classic engine.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";
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

// Setup test collection.
assert.commandWorked(coll.insertMany([
    {groupKey: 1, val: "a"},
    {groupKey: 1, val: "b"},
    {groupKey: 2, val: "c"},
    {groupKey: 2, val: "d"},
]));

runMemoryStatsTest(db,
                   collName,
                   [{$group: {_id: "$groupKey", values: {$push: "$val"}}}] /*pipeline*/,
                   "memory stats group test" /*pipelineComment*/,
                   "group" /*stageName*/,
                   2 /*expectedNumGetMores*/);

// Clean up.
db[collName].drop();
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
