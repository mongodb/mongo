/**
 * Test that $setWindowFields succeeds if it needs to spill to disk with readConcern snapshot and in
 * transactions.
 * @tags: [
 *   requires_replication,
 *   uses_transactions,
 *   uses_snapshot_read_concern,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const rstPrimary = rst.getPrimary();
const testDB = rstPrimary.getDB(jsTestName() + "_db");
const coll = testDB[jsTestName() + "_coll"];
coll.drop();

function checkProfilerForDiskWrite(dbToCheck) {
    const profileObj = getLatestProfilerEntry(dbToCheck, {usedDisk: true});
    // Verify that this was a $setWindowFields stage as expected.
    if (profileObj.hasOwnProperty("originatingCommand")) {
        const firstStage = profileObj.originatingCommand.pipeline[0];
        assert(firstStage.hasOwnProperty("$setWindowFields") ||
               firstStage.hasOwnProperty("$lookup"));
    } else if (profileObj.hasOwnProperty("command")) {
        const firstStage = profileObj.command.pipeline[0];
        assert(firstStage.hasOwnProperty("$setWindowFields") ||
               firstStage.hasOwnProperty("$lookup"));
    } else {
        assert(false, "Profiler should have had command field", profileObj);
    }
}
const documents = [];
for (let i = 0; i < 30; i++) {
    documents.push({_id: i, val: i, partition: 1});
    documents.push({_id: i + 30, val: i, partition: 2});
}
assert.commandWorked(coll.insert(documents));

setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(testDB.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       1500);
const rsStatus = rst.status();
const lastClusterTime = rsStatus.optimes.lastCommittedOpTime.ts;
const lowerBound = -21;
const upperBound = 21;
let pipeline = [
    {
        $setWindowFields: {
            partitionBy: "$partition",
            sortBy: {partition: 1, val: 1},
            output: {sum: {$sum: "$val", window: {documents: [lowerBound, upperBound]}}}
        }
    },
    {$sort: {val: 1, partition: 1}},
];
let aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: pipeline,
    allowDiskUse: true,
    readConcern: {level: "snapshot", atClusterTime: lastClusterTime},
    cursor: {}
};

function resetProfiler() {
    testDB.setProfilingLevel(0);
    testDB.system.profile.drop();
    testDB.setProfilingLevel(2);
}

// Run outside of a transaction.
resetProfiler();
let commandResult = assert.commandWorked(testDB.runCommand(aggregationCommand));
checkProfilerForDiskWrite(testDB);
let arrayResult = commandResult.cursor.firstBatch;
let expected = [];

let curSum = (21) * (11);
for (let i = 0; i < 30; i++) {
    expected.push({_id: i, val: i, partition: 1, sum: curSum});
    expected.push({_id: i + 30, val: i, partition: 2, sum: curSum});
    // Subtract the beginning of the window. Add because the lowerBound is negative.
    curSum = curSum - Math.max(0, i + lowerBound);
    // Add the end of the window.
    if (i < 29 - upperBound) {
        curSum = curSum + i + upperBound + 1;
    }
}
assertArrayEq({actual: arrayResult, expected: expected});

// Make sure that a $setWindowFields in a subpipeline with readConcern snapshot succeeds.
const lookupPipeline = [{$lookup: {from: coll.getName(), pipeline: pipeline, as: "newField"}}];
aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: lookupPipeline,
    allowDiskUse: true,
    readConcern: {level: "snapshot", atClusterTime: lastClusterTime},
    cursor: {}
};
// We're running the same setWindowFields multiple times. Just check if the command doesn't
// crash the server instead of checking results from here on out.
assert.commandWorked(testDB.runCommand(aggregationCommand));

// Repeat in a transaction. Don't check for disk writes, as can't query the profiler in a
// transaction.
let session = rstPrimary.startSession();
session.startTransaction({readConcern: {level: "snapshot"}});
const sessionDB = session.getDatabase(testDB.getName());
const sessionColl = sessionDB.getCollection(coll.getName());
aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: pipeline,
    allowDiskUse: true,
    cursor: {},
};
assert.commandWorked(sessionColl.runCommand(aggregationCommand));
// Restart transaction.
session.abortTransaction();
session.startTransaction({readConcern: {level: "snapshot"}});
// Repeat the subpipeline test in a transaction.
aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: lookupPipeline,
    allowDiskUse: true,
    cursor: {}
};
assert.commandWorked(sessionColl.runCommand(aggregationCommand));
session.abortTransaction();
rst.stopSet();
