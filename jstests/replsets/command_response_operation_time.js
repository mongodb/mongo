/**
 * Tests that reads and writes in a replica set return the correct operationTime for their
 * read/write concern level. Majority reads and writes return the last committed optime's timestamp
 * and local reads and writes return the last applied optime's timestamp.
 * @tags: [requires_majority_read_concern]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

function assertCorrectOperationTime(operationTime, expectedTimestamp, opTimeType) {
    assert.eq(
        0,
        timestampCmp(operationTime, expectedTimestamp),
        "operationTime in command response, " +
            tojson(operationTime) +
            ", does not equal the last " +
            opTimeType +
            " timestamp, " +
            tojson(expectedTimestamp),
    );
}

let name = "command_response_operation_time";

let replTest = new ReplSetTest({name: name, nodes: 3, waitForKeys: true});
replTest.startSet();
replTest.initiate();

let res, statusRes;
let testDB = replTest.getPrimary().getDB(name);

jsTestLog("Executing majority write.");
res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 1}], writeConcern: {w: "majority"}}));
statusRes = assert.commandWorked(testDB.adminCommand({replSetGetStatus: 1}));
assertCorrectOperationTime(res.operationTime, statusRes.optimes.lastCommittedOpTime.ts, "committed");

jsTestLog("Executing local write.");
res = assert.commandWorked(testDB.runCommand({insert: "foo", documents: [{x: 2}]}));
statusRes = assert.commandWorked(testDB.adminCommand({replSetGetStatus: 1}));
assertCorrectOperationTime(res.operationTime, statusRes.optimes.appliedOpTime.ts, "applied");

replTest.awaitLastOpCommitted();

jsTestLog("Executing majority read.");
res = assert.commandWorked(testDB.runCommand({find: "foo", filter: {x: 1}, readConcern: {level: "majority"}}));
statusRes = assert.commandWorked(testDB.adminCommand({replSetGetStatus: 1}));
assertCorrectOperationTime(res.operationTime, statusRes.optimes.lastCommittedOpTime.ts, "committed");

jsTestLog("Executing local read.");
res = assert.commandWorked(testDB.runCommand({find: "foo", filter: {x: 1}}));
statusRes = assert.commandWorked(testDB.adminCommand({replSetGetStatus: 1}));
assertCorrectOperationTime(res.operationTime, statusRes.optimes.appliedOpTime.ts, "applied");

replTest.stopSet();
