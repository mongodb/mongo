/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for pipelines that
 * use SBE's UniqueStage and UniqueRoaringStage.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   requires_fcv_83,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

if (!checkSbeFullyEnabled(db)) {
    // This test is specifically for the SBE "unique" stage, so don't run the test if SBE is not
    // enabled.
    MongoRunner.stopMongod(conn);
    jsTest.log.info("Skipping test for SBE 'unique' stage when SBE is not enabled.");
    quit();
}

// The tests expect that memory metrics appear right after memory is used. Decrease the threshold
// for rate-limiting writes to CurOp. Otherwise, we may report no memory usage if the memory used is
// less than the limit.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 16}));

const normalColl = db[jsTestName() + "_normal"];
normalColl.drop();
const clusteredColl = db[jsTestName() + "_clustered"];
clusteredColl.drop();

// Base document data with integer _id.
const docs = [
    {
        _id: 0,
        category: "A",
        status: "active",
        priority: 1,
        value: 10,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 1,
        category: "B",
        status: "inactive",
        priority: 2,
        value: 20,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 2,
        category: "A",
        status: "pending",
        priority: 1,
        value: 30,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 3,
        category: "C",
        status: "active",
        priority: 3,
        value: 40,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 4,
        category: "B",
        status: "active",
        priority: 2,
        value: 50,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 5,
        category: "A",
        status: "active",
        priority: 1,
        value: 60,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 6,
        category: "D",
        status: "inactive",
        priority: 4,
        value: 70,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 7,
        category: "E",
        status: "pending",
        priority: 3,
        value: 80,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 8,
        category: "F",
        status: "active",
        priority: 4,
        value: 90,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
    {
        _id: 9,
        category: "G",
        status: "inactive",
        priority: 5,
        value: 100,
        str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    },
];

// Pipeline that triggers unique stage via OR predicate. This predicate matches 5 unique rows.
const pipeline = [
    {
        $match: {
            $or: [
                {category: "A", priority: 1, str: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                {status: "active", value: {$gte: 50}},
            ],
        },
    },
];

// Set up a clustered collection to force usage of the more general UniqueStage.
clusteredColl.drop();
assert.commandWorked(db.createCollection(clusteredColl.getName(), {clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandWorked(clusteredColl.insertMany(docs));
assert.commandWorked(clusteredColl.createIndex({category: 1, priority: 1}));
assert.commandWorked(clusteredColl.createIndex({status: 1, value: 1}));

// With an unclustered collection, deduping will be done by UniqueRoaringStage.
normalColl.drop();
assert.commandWorked(normalColl.insertMany(docs));
assert.commandWorked(normalColl.createIndex({category: 1, priority: 1}));
assert.commandWorked(normalColl.createIndex({status: 1, value: 1}));

// Test UniqueStage with clustered collection.
jsTest.log.info("Testing UniqueStage with clustered collection");
runMemoryStatsTest({
    db: db,
    collName: clusteredColl.getName(),
    commandObj: {
        aggregate: clusteredColl.getName(),
        pipeline: pipeline,
        cursor: {batchSize: 2},
        comment: "memory stats unique general test",
        allowDiskUse: false,
    },
    stageName: "unique",
    expectedNumGetMores: 2,
});

// Test UniqueRoaringStage with normal collection.
jsTest.log.info("Testing UniqueRoaringStage with normal collection");
runMemoryStatsTest({
    db: db,
    collName: normalColl.getName(),
    commandObj: {
        aggregate: normalColl.getName(),
        pipeline: pipeline,
        cursor: {batchSize: 2},
        comment: "memory stats unique roaring test",
        allowDiskUse: false,
    },
    stageName: "unique_roaring",
    expectedNumGetMores: 2,
});

// Clean up.
normalColl.drop();
clusteredColl.drop();
MongoRunner.stopMongod(conn);
