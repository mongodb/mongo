/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for pipelines
 * that sort across different sort implementations: DocumentSourceSort, SBE Sort, and PlanStage
 * Sort.
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
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const collName = jsTestName();
const coll = db[collName];
db[collName].drop();

// The tests expect that memory metrics appear right after memory is used. Decrease the threshold
// for rate-limiting writes to CurOp. Otherwise, we may report no memory usage if the memory used <
// limit.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 256}));

const bigStr = Array(1025).toString(); // 1KB of ','
const lowMaxMemoryLimit = 5000;
const nDocs = 50;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, a: i, b: i, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

// Since this test is run against all execution engine variants, we can save compute by only
// checking the relevant stages for the variant; for example, we will get test coverage for SBE Sort
// on trySbeEngine evergreen variants, so there is no need to force SBE execution on variants where
// SBE is not enabled by default.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
let configs;
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info("SBE is fully enabled.");
    configs = [
        {
            name: "SBE Sort",
            pipelineFn: (pipeline) => {
                // Add $_internalInhibitOptimization to prevent $sort pushdown so the pipeline uses
                // SBE.
                pipeline.unshift({$_internalInhibitOptimization: {}});
            },
            stageName: "sort", // SBE sort stage appears without the dollar sign
        },
    ];
} else {
    jsTest.log.info("Classic engine is enabled.");
    configs = [
        {
            name: "DocumentSourceSort",
            pipelineFn: (pipeline) => {
                // Add $_internalInhibitOptimization to prevent $sort pushdown to find, allowing to
                // test DocumentSourceSort
                pipeline.unshift({$_internalInhibitOptimization: {}});
            },
            stageName: "$sort",
        },
        {
            name: "PlanStage Sort",
            stageName: "SORT", // PlanStage sort appears as uppercase SORT in explain output
        },
    ];
}

for (const config of configs) {
    describe(config.name, () => {
        let pipeline, pipelineWithLimit;

        // Setup for this configuration.
        before(() => {
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
                    allowDiskUse: false,
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
                    allowDiskUse: false,
                },
                stageName: config.stageName,
                expectedNumGetMores: 5,
            });
        });

        it("should track memory for sort with spilling", () => {
            // Set maxMemory low to force spill to disk.
            const originalMemoryLimit = assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryMaxBlockingSortMemoryUsageBytes: lowMaxMemoryLimit,
                }),
            );

            runMemoryStatsTest({
                db: db,
                collName: collName,
                commandObj: {
                    aggregate: collName,
                    pipeline: pipeline,
                    cursor: {batchSize: 10},
                    comment: "memory stats sort spilling test",
                    allowDiskUse: true,
                },
                stageName: config.stageName,
                expectedNumGetMores: 5,
                // Since we spill to disk when adding to the sorter, we don't expect to see
                // inUseTrackedMemBytes populated as it should be 0 on each operation.
                skipInUseTrackedMemBytesCheck: true,
            });

            // Set maxMemory back to the original value.
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryMaxBlockingSortMemoryUsageBytes: originalMemoryLimit.was,
                }),
            );
        });
    });
}

// Clean up.
after(() => {
    db[collName].drop();
    MongoRunner.stopMongod(conn);
});
