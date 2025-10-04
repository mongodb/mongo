/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for operations
 * using the spool plan stage.
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
 * # The spool stage is only used with updates/deletes on time-series collections.
 * featureFlagTimeseriesUpdatesSupport,
 * assumes_against_mongod_not_mongos,
 * ]
 */
import {runMemoryStatsTestForTimeseriesUpdateCommand} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const collName = jsTestName();
const coll = db[collName];

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
assert.commandWorked(db.adminCommand({setParameter: 1, allowDiskUseByDefault: true}));

// Set up test collection.
const dateTime = ISODate("2021-07-12T16:00:00Z");
const buckets = ["A", "B", "C", "D", "E", "F", "G"];
const numDocsPerBucket = 4;

function setUpCollectionForTest() {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "time", metaField: "m"}}));

    let docs = [];
    for (const bucket of buckets) {
        for (let i = 0; i < numDocsPerBucket; ++i) {
            docs.push({"time": dateTime, "m": bucket, str: i % 2 == 0 ? "even" : "odd"});
        }
    }
    assert.commandWorked(coll.insert(docs));
}

{
    const updateCommand = {
        update: collName,
        updates: [{q: {str: "even"}, u: {$set: {str: "not even"}}, multi: true}],
        comment: "memory stats spool test",
    };
    jsTest.log.info("Running spool test with no spilling: " + tojson(updateCommand));

    const memoryLimitBytes = 100 * 1024 * 1024;
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxSpoolMemoryUsageBytes: memoryLimitBytes}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxSpoolDiskUsageBytes: 10 * memoryLimitBytes}),
    );

    setUpCollectionForTest();
    runMemoryStatsTestForTimeseriesUpdateCommand({db: db, collName: collName, commandObj: updateCommand});
}

{
    const updateCommand = {
        update: collName,
        updates: [{q: {str: "even"}, u: {$set: {str: "not even"}}, multi: true}],
        comment: "memory stats spool test with spilling",
    };
    jsTest.log.info("Running spool test with spilling: " + tojson(updateCommand));

    // The spool stage will spill 32-byte record ids in this instance. Set a limit just under that
    // size so that we will need to spill on every other record.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxSpoolMemoryUsageBytes: 50}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxSpoolDiskUsageBytes: 10 * 50}));

    setUpCollectionForTest();

    runMemoryStatsTestForTimeseriesUpdateCommand({db: db, collName: collName, commandObj: updateCommand});
}

// Clean up.
db[collName].drop();
MongoRunner.stopMongod(conn);
