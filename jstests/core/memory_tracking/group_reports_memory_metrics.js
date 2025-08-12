/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with $group.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * requires_fcv_82,
 * assumes_against_mongod_not_mongos,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// Setup test collection.
assert.commandWorked(coll.insertMany([
    {groupKey: 1, val: "a"},
    {groupKey: 1, val: "b"},
    {groupKey: 2, val: "c"},
    {groupKey: 2, val: "d"},
]));
const pipeline = [{$group: {_id: "$groupKey", values: {$push: "$val"}}}];
const pipelineWithLimit = [{$group: {_id: "$groupKey", values: {$push: "$val"}}}, {$limit: 2}];

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;
// 'forceIncreasedSpilling' should not be enabled for the following tests.
const kOriginalForceIncreasedSpilling =
    assert
        .commandWorked(db.adminCommand(
            {setParameter: 1, internalQuerySlotBasedExecutionHashAggIncreasedSpilling: "never"}))
        .was;
const configs = [
    {
        name: "DocumentSourceGroup",
        framework: "forceClassicEngine",
        stageName: "$group",
        knobName: "internalDocumentSourceGroupMaxMemoryBytes",
    },
    {
        name: "SBE Group",
        framework: "trySbeEngine",
        stageName: "group",  // SBE group stage appears without the dollar sign
        knobName: "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill",
    },
];

try {
    for (const config of configs) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: config.framework}));

        {
            runMemoryStatsTest({
                db: db,
                collName: collName,
                commandObj: {
                    aggregate: collName,
                    pipeline: pipeline,
                    comment: "memory stats group test",
                    cursor: {batchSize: 1},
                    allowDiskUse: false
                },
                stageName: config.stageName,
                expectedNumGetMores: 2,
            });
        }

        {
            runMemoryStatsTest({
                db: db,
                collName: collName,
                commandObj: {
                    aggregate: collName,
                    pipeline: pipelineWithLimit,
                    cursor: {batchSize: 1},
                    comment: "memory stats group limit test",
                    allowDiskUse: false
                },
                stageName: config.stageName,
                expectedNumGetMores: 1,
            });
        }

        {
            const lowMaxMemoryLimit = 100;
            const memoryLimitKnob = config.knobName;
            // Set maxMemory low to force spill to disk.
            const originalMemoryLimit = assert.commandWorked(
                db.adminCommand({setParameter: 1, [memoryLimitKnob]: lowMaxMemoryLimit}));

            try {
                runMemoryStatsTest({
                    db: db,
                    collName: collName,
                    commandObj: {
                        aggregate: collName,
                        pipeline: pipeline,
                        cursor: {batchSize: 1},
                        comment: "memory stats group spilling test",
                        allowDiskUse: true
                    },
                    stageName: config.stageName,
                    expectedNumGetMores: 2,
                    // Since we spill, we don't expect to see
                    // inUseTrackedMemBytes populated as it should be 0 on each operation.
                    skipInUseTrackedMemBytesCheck: true,
                });
            } finally {
                // Set maxMemory back to the original value.
                assert.commandWorked(
                    db.adminCommand({setParameter: 1, [memoryLimitKnob]: originalMemoryLimit.was}));
            }
        }
    }
} finally {
    db[collName].drop();
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashAggIncreasedSpilling: kOriginalForceIncreasedSpilling
    }));
}
