/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for aggregations
 * with hash lookup.
 *
 * @tags: [
 * requires_profiling,
 * requires_getmore,
 * # The profiler is only run against a mongod.
 * assumes_against_mongod_not_mongos,
 * # The test queries the system.profile collection so it is not compatible with initial sync
 * # since an initial sync may insert unexpected operations into the profile collection.
 * queries_system_profile_collection,
 * # The test runs the profile and getLog commands, which are not supported in Serverless.
 * command_not_supported_in_serverless,
 * requires_fcv_82,
 * ]
 */
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

// Get the current value of the query framework server parameter so we can restore it at the end of
// the test. Otherwise, the tests run after this will be affected.
const kOriginalInternalQueryFrameworkControl =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl;
const kOriginalMemoryLimit =
    assert
        .commandWorked(db.adminCommand({
            getParameter: 1,
            internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1
        }))
        .internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill;

const stageName = "hash_lookup";

// Set up test collections.
const employees = db[jsTestName() + "_employees"];
const people = db[jsTestName() + "_people"];
employees.drop();
people.drop();

const employeeDocs = [
    {employeeId: 22001, personId: 1002},
    {employeeId: 21001, personId: 1000},
    {employeeId: 20010, personId: 1002},
    {employeeId: 22021, personId: 1001},
    {employeeId: 17301, personId: 1003},
    {employeeId: 21022, personId: 1004},
    {employeeId: 20020, personId: 1003},
    {employeeId: 18020, personId: 1004},
];

const peopleDocs = [
    {pID: 1000, name: "Aaron"},
    {pID: 1001, name: "Arun"},
    {pID: 1002, name: "Chris"},
    {pID: 1003, name: "Erin Z"},
    {pID: 1004, name: "Erin L"}
];

assert.commandWorked(employees.insertMany(employeeDocs));
assert.commandWorked(people.insertMany(peopleDocs));

try {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

    {
        const pipeline = [{
            $lookup:
                {from: people.getName(), localField: "personId", foreignField: "pID", as: "matched"}
        }];
        jsTest.log.info("Running basic pipeline test: " + tojson(pipeline));

        runMemoryStatsTest({
            db: db,
            collName: employees.getName(),
            commandObj: {
                aggregate: employees.getName(),
                pipeline: pipeline,
                cursor: {batchSize: 1},
                comment: "memory stats lookup test",
            },
            stageName,
            expectedNumGetMores: 7,
        });
    }

    {
        const pipelineWithLimit = [
            {$lookup: {from: people.getName(), localField: "personId", foreignField: "pID", as: "matches"}},
            {$limit: 2}
        ];
        jsTest.log.info("Running pipeline with $limit: " + tojson(pipelineWithLimit));

        runMemoryStatsTest({
            db: db,
            collName: employees.getName(),
            commandObj: {
                aggregate: employees.getName(),
                pipeline: pipelineWithLimit,
                cursor: {batchSize: 1},
                comment: "memory stats lookup with limit test",
            },
            stageName,
            expectedNumGetMores: 1,
            skipInUseMemBytesCheck: true,  // $limit will force execution to stop early
        });
    }

    {
        const pipeline = [{
            $lookup:
                {from: people.getName(), localField: "personId", foreignField: "pID", as: "matches"}
        }];
        jsTest.log.info("Running pipeline that will spill: " + tojson(pipeline));

        // Set a low memory limit to force spilling to disk.
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 100
        }));

        runMemoryStatsTest({
            db: db,
            collName: employees.getName(),
            commandObj: {
                aggregate: employees.getName(),
                pipeline: pipeline,
                cursor: {batchSize: 1},
                comment: "memory stats lookup with spilling test",
            },
            stageName,
            expectedNumGetMores: 7,
            skipInUseMemBytesCheck: true,  // Since we spill, we don't expect to see inUseMemBytes
            // populated, as it should be 0 on each operation.
        });
    }
} finally {
    // Clean up
    employees.drop();
    people.drop();
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill:
            kOriginalMemoryLimit
    }));
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryFrameworkControl: kOriginalInternalQueryFrameworkControl}));
}
