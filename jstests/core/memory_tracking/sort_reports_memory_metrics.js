/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for pipelines
 * that sort across different sort implementations: DocumentSourceSort, SBE Sort, and PlanStage
 * Sort. Due to requires_profiling, this test should not run with sharded clusters.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * assumes_against_mongod_not_mongos,
 * requires_fcv_82,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;

const bigStr = Array(1025).toString();  // 1KB of ','
const lowMaxMemoryLimit = 5000;
const nDocs = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

const configs = [
    {
        name: "DocumentSourceSort",
        framework: "forceClassicEngine",
        pipelineFn: (pipeline) => {
            // Add $_internalInhibitOptimization to prevent $sort pushdown to find, allowing to test
            // DocumentSourceSort
            pipeline.unshift({$_internalInhibitOptimization: {}});
        },
        stageName: "$sort",
    },
    {
        name: "SBE Sort",
        framework: "trySbeEngine",
        pipelineFn: (pipeline) => {
            // Add $_internalInhibitOptimization to prevent $sort pushdown, keeping it in SBE
            pipeline.unshift({$_internalInhibitOptimization: {}});
        },
        stageName: "sort",  // SBE sort stage appears without the dollar sign
    },
    {
        name: "PlanStage Sort",
        framework: "forceClassicEngine",
        stageName: "SORT",  // PlanStage sort appears as uppercase SORT in explain output
    }
];

for (const config of configs) {
    describe(config.name, () => {
        let pipeline, pipelineWithLimit;

        // Setup for this configuration
        before(() => {
            assert.commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryFrameworkControl: config.framework}));

            // Define base pipelines without inhibit optimization
            pipeline = [{$sort: {_id: 1, b: -1}}];
            pipelineWithLimit = [{$sort: {_id: 1, b: -1}}, {$limit: nDocs / 10}];

            // Let the configuration modify each pipeline if needed
            if (config.pipelineFn) {
                config.pipelineFn(pipeline);
                config.pipelineFn(pipelineWithLimit);
            }
        });

        it("should track memory for basic sort", () => {
            runMemoryStatsTest({
                db: db,
                collName: collName,
                commandObj: {
                    aggregate: collName,
                    pipeline: pipeline,
                    cursor: {batchSize: 10},
                    comment: "memory stats sort test",
                    allowDiskUse: false
                },
                stageName: config.stageName,
                expectedNumGetMores: 5,
            });
        });

        it("should track memory for sort with limit", () => {
            runMemoryStatsTest({
                db: db,
                collName: collName,
                commandObj: {
                    aggregate: collName,
                    pipeline: pipelineWithLimit,
                    cursor: {batchSize: 1},
                    comment: "memory stats sort limit test",
                    allowDiskUse: false
                },
                stageName: config.stageName,
                expectedNumGetMores: 5,
            });
        });

        it("should track memory for sort with spilling", () => {
            // Set maxMemory low to force spill to disk.
            const originalMemoryLimit = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryMaxBlockingSortMemoryUsageBytes: lowMaxMemoryLimit
            }));

            try {
                runMemoryStatsTest({
                    db: db,
                    collName: collName,
                    commandObj: {
                        aggregate: collName,
                        pipeline: pipeline,
                        cursor: {batchSize: 10},
                        comment: "memory stats sort spilling test",
                        allowDiskUse: true
                    },
                    stageName: config.stageName,
                    expectedNumGetMores: 5,
                    // Since we spill to disk when adding to the sorter, we don't expect to see
                    // inUseMemBytes populated as it should be 0 on each operation.
                    skipInUseMemBytesCheck: true,
                });
            } finally {
                // Set maxMemory back to the original value.
                assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    internalQueryMaxBlockingSortMemoryUsageBytes: originalMemoryLimit.was
                }));
            }
        });
    });
}

// Clean up.
after(() => {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
});
