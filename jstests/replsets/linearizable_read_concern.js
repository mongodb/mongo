/*
 * This test creates a 3 node replica set. The test first sends
 * a regular linearizable read command which should succeed. Then the test
 * examines linearizable read parsing abilities by sending a linearizable
 * read command to a secondary and then to the primary with an 'afterOpTime'
 * field, both of which should fail. The test then starts to test the actual
 * functionality of linearizable reads by creating a network partition between the primary
 * and the other two nodes and then sending in a linearizable read command.
 * Finally we test whether the linearizable read command will block forever
 * by issuing a linearizable read command in another thread on the still
 * partitioned primary and then making the primary step down in the main
 * thread after finding the issued noop. The secondary thread should throw
 * an exception and exit.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getLatestOp} from "jstests/replsets/rslib.js";

let send_linearizable_read = function () {
    // The primary will step down and throw an exception, which is expected.
    let coll = db.getSiblingDB("test").foo;
    jsTestLog("Sending in linearizable read in secondary thread");
    // 'hello' ensures that the following command fails (and returns a response rather than
    // an exception) before its connection is cut because of the primary step down. Refer to
    // SERVER-24574.
    assert.commandWorked(coll.runCommand({hello: 1, hangUpOnStepDown: false}));
    assert.commandFailedWithCode(
        coll.runCommand({"find": "foo", readConcern: {level: "linearizable"}, maxTimeMS: 60000}),
        ErrorCodes.InterruptedDueToReplStateChange,
    );
};

let num_nodes = 3;
let name = "linearizable_read_concern";
let replTest = new ReplSetTest({name: name, nodes: num_nodes, useBridge: true});
let config = replTest.getReplSetConfig();

// Increased election timeout to avoid having the primary step down while we are
// testing linearizable functionality on an isolated primary.
config.settings = {
    electionTimeoutMillis: 60000,
};

replTest.startSet();
replTest.initiate(config);

let primary = replTest.getPrimary();
let secondaries = replTest.getSecondaries();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Without a sync source the heartbeat interval will be half of the election timeout, 30
// seconds. It thus will take almost 30 seconds for the secondaries to set the primary as
// their sync source and begin replicating.
replTest.awaitReplication();
// Do a write to have something to read.
assert.commandWorked(
    primary
        .getDB("test")
        .foo.insert({"number": 7}, {"writeConcern": {"w": "majority", "wtimeout": ReplSetTest.kDefaultTimeoutMS}}),
);

jsTestLog("Testing linearizable readConcern parsing");
// This command is sent to the primary, and the primary is fully connected so it should work.
let goodRead = assert.commandWorked(
    primary.getDB("test").runCommand({"find": "foo", readConcern: {level: "linearizable"}, "maxTimeMS": 60000}),
);
assert.eq(goodRead.cursor.firstBatch[0].number, 7);

// This fails because you cannot have a linearizable read command sent to a secondary.
let badCmd = assert.commandFailed(
    secondaries[0].getDB("test").runCommand({"find": "foo", readConcern: {level: "linearizable"}, "maxTimeMS": 60000}),
);

assert.eq(badCmd.errmsg, "cannot satisfy linearizable read concern on non-primary node");
assert.eq(badCmd.code, ErrorCodes.NotWritablePrimary);

// This fails because you cannot specify 'afterOpTime' for linearizable read.
let opTimeCmd = assert.commandFailed(
    primary.getDB("test").runCommand({
        "find": "foo",
        readConcern: {level: "linearizable", "afterOpTime": {ts: Timestamp(1, 2), t: 1}},
        "maxTimeMS": 60000,
    }),
);
assert.eq(opTimeCmd.errmsg, "afterOpTime not compatible with linearizable read concern");
assert.eq(opTimeCmd.code, ErrorCodes.FailedToParse);

// A $out aggregation is not allowed with readConcern level "linearizable".
assert.throwsWithCode(
    () => primary.getDB("test").foo.aggregate([{$out: "out"}], {readConcern: {level: "linearizable"}}),
    ErrorCodes.InvalidOptions,
);

// A $merge aggregation is not allowed with readConcern level "linearizable".
assert.throwsWithCode(
    () =>
        primary
            .getDB("test")
            .foo.aggregate([{$merge: {into: "out", whenMatched: "replace", whenNotMatched: "insert"}}], {
                readConcern: {level: "linearizable"},
            }),
    ErrorCodes.InvalidOptions,
);

primary = replTest.getPrimary();

jsTestLog("Starting linearizablility testing");

const cursorId = assert.commandWorked(
    primary.getDB("test").runCommand({"find": "foo", readConcern: {level: "linearizable"}, batchSize: 0}),
).cursor.id;
jsTestLog("Setting up partitions such that the primary is isolated: [Secondary-Secondary] [Primary]");
secondaries[0].disconnect(primary);
secondaries[1].disconnect(primary);

jsTestLog("Testing to make sure that linearizable getMores will time out when the primary is isolated.");
assert.commandWorked(primary.getDB("test").foo.insert({_id: 0, x: 0}));
assert.commandFailedWithCode(
    primary.getDB("test").runCommand({"getMore": cursorId, collection: "foo", batchSize: 1}),
    ErrorCodes.LinearizableReadConcernError,
);

jsTestLog("Test that a linearizable read will timeout when the primary is isolated.");
let findResult = primary
    .getDB("test")
    .runCommand({"find": "foo", "readConcern": {level: "linearizable"}, "maxTimeMS": 3000});
assert.commandFailedWithCode(findResult, ErrorCodes.MaxTimeMSExpired);

jsTestLog("Testing to make sure linearizable read command does not block forever.");

// Get last noop Optime before sending the linearizable read command
// to ensure that we are waiting for the most recent noop write.
let lastOpTimestamp = getLatestOp(primary).ts;

let parallelShell = startParallelShell(send_linearizable_read, primary.port);
// Sending a linearizable read implicitly replicates a noop to the secondaries. We need to find
// the most recently issued noop to ensure that we call stepdown during the recently
// issued linearizable read and not before the read (in the separate thread) has been called.
jsTestLog("Checking end of oplog for noop");
assert.soon(function () {
    let isEarlierTimestamp = function (ts1, ts2) {
        if (ts1.getTime() == ts2.getTime()) {
            return ts1.getInc() < ts2.getInc();
        }
        return ts1.getTime() < ts2.getTime();
    };
    let latestOp = getLatestOp(primary);
    if (latestOp.op == "n" && isEarlierTimestamp(lastOpTimestamp, latestOp.ts)) {
        return true;
    }

    return false;
});
assert.eq(primary, replTest.getPrimary(), "Primary unexpectedly changed mid test.");
jsTestLog("Making Primary step down");
assert.commandWorked(primary.adminCommand({"replSetStepDown": 100, secondaryCatchUpPeriodSecs: 0, "force": true}));
parallelShell();
replTest.stopSet();
