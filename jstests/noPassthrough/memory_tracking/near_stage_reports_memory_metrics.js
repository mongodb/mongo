/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for the
 * GEO_NEAR_2DSPHERE stage.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_90,
 * ]
 */

import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const kDocCount = 100;
const docs = Array.from({length: kDocCount}, (_, i) => {
    const lng = (i % 360) - 180;
    const lat = (Math.floor(i / 360) % 180) - 90;
    return {_id: i, loc: {type: "Point", coordinates: [lng, lat]}};
});
assert.commandWorked(coll.insertMany(docs));
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

const pipeline = [{$geoNear: {near: {type: "Point", coordinates: [0, 0]}, distanceField: "dist"}}];

// Check if the query uses GEO_NEAR_2DSPHERE before proceeding. This stage is only used by the
// classic engine.
const preliminaryExplain = coll.explain("executionStats").aggregate(pipeline);
const nearStages = getAggPlanStages(preliminaryExplain, "GEO_NEAR_2DSPHERE");
if (nearStages.length === 0) {
    jsTest.log.info(
        "Skipping test: query did not use GEO_NEAR_2DSPHERE stage. This stage is only used by the classic engine.",
    );
    coll.drop();
    MongoRunner.stopMongod(conn);
    quit();
}

const kBatchSize = 10;
runMemoryStatsTest({
    db,
    collName,
    commandObj: {
        aggregate: collName,
        pipeline,
        comment: "memory stats near stage test",
        allowDiskUse: false,
        cursor: {batchSize: kBatchSize},
    },
    stageName: "GEO_NEAR_2DSPHERE",
    expectedNumGetMores: kDocCount / kBatchSize - 1,
    // near stage still holds the memory used for the record id deduplication at the last batch.
    checkInUseTrackedMemBytesResets: false,
});

// Clean up.
coll.drop();
MongoRunner.stopMongod(conn);
