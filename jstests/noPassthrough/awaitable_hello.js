/**
 * Tests the maxAwaitTimeMS and topologyVersion parameters of the hello command, and its aliases,
 * isMaster and ismaster.
 * @tags: [requires_replication]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");

// runTest takes in the hello command or its aliases, isMaster and ismaster.
function runTest(db, cmd) {
    // Check the command response contains a topologyVersion even if maxAwaitTimeMS and
    // topologyVersion are not included in the request.
    const res = assert.commandWorked(db.runCommand(cmd));
    assert(res.hasOwnProperty("topologyVersion"), tojson(res));

    const topologyVersionField = res.topologyVersion;
    assert(topologyVersionField.hasOwnProperty("processId"), tojson(topologyVersionField));
    assert(topologyVersionField.hasOwnProperty("counter"), tojson(topologyVersionField));

    // Check that the command succeeds when passed a valid topologyVersion and maxAwaitTimeMS. In
    // this case, use the topologyVersion from the previous command response. The topologyVersion
    // field is expected to be of the form {processId: <ObjectId>, counter: <long>}.
    assert.commandWorked(
        db.runCommand({[cmd]: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 0}));
    // Ensure the command waits for at least maxAwaitTimeMS before returning.
    let now = new Date();
    assert.commandWorked(
        db.runCommand({[cmd]: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 2000}));
    let commandDuration = new Date() - now;
    // Allow for some clock imprecision between the server and the jstest.
    assert.gte(
        commandDuration,
        1000,
        cmd + ` command should have taken at least 1000ms, but completed in ${commandDuration}ms`);

    // Check that when a different processId is given, the server responds immediately.
    now = new Date();
    assert.commandWorked(db.runCommand({
        [cmd]: 1,
        topologyVersion: {processId: ObjectId(), counter: topologyVersionField.counter},
        maxAwaitTimeMS: 2000
    }));
    commandDuration = new Date() - now;
    assert.lt(
        commandDuration,
        1000,
        cmd + ` command should have taken at most 1000ms, but completed in ${commandDuration}ms`);

    // Check that when a different processId is given, a higher counter is allowed.
    assert.commandWorked(db.runCommand({
        [cmd]: 1,
        topologyVersion:
            {processId: ObjectId(), counter: NumberLong(topologyVersionField.counter + 1)},
        maxAwaitTimeMS: 0
    }));

    // Check that when the same processId is given, a higher counter is not allowed.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: {
            processId: topologyVersionField.processId,
            counter: NumberLong(topologyVersionField.counter + 1)
        },
        maxAwaitTimeMS: 0
    }),
                                 [31382, 51761, 51764]);

    // Check that passing a topologyVersion not of type object fails.
    assert.commandFailedWithCode(
        db.runCommand({[cmd]: 1, topologyVersion: "topology_version_string", maxAwaitTimeMS: 0}),
        10065);

    // Check that a topologyVersion with an invalid processId and valid counter fails.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: {processId: "pid1", counter: topologyVersionField.counter},
        maxAwaitTimeMS: 0
    }),
                                 ErrorCodes.TypeMismatch);

    // Check that a topologyVersion with a valid processId and invalid counter fails.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: {processId: topologyVersionField.processId, counter: 0},
        maxAwaitTimeMS: 0
    }),
                                 ErrorCodes.TypeMismatch);

    // Check that a topologyVersion with a valid processId but missing counter fails.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: {processId: topologyVersionField.processId},
        maxAwaitTimeMS: 0
    }),
                                 40414);

    // Check that a topologyVersion with a missing processId and valid counter fails.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: {counter: topologyVersionField.counter},
        maxAwaitTimeMS: 0
    }),
                                 40414);

    // Check that a topologyVersion with a valid processId and negative counter fails.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: {processId: topologyVersionField.processId, counter: NumberLong("-1")},
        maxAwaitTimeMS: 0
    }),
                                 [31372, 51758]);

    // Check that the command fails if there is an extra field in its topologyVersion.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: {
            processId: topologyVersionField.processId,
            counter: topologyVersionField.counter,
            randomField: "I should cause an error"
        },
        maxAwaitTimeMS: 0
    }),
                                 40415);

    // A client following the awaitable hello/isMaster protocol must include topologyVersion in
    // their request if and only if they include maxAwaitTimeMS. Check that the command fails if
    // there is a topologyVersion but no maxAwaitTimeMS field.
    assert.commandFailedWithCode(db.runCommand({[cmd]: 1, topologyVersion: topologyVersionField}),
                                 [31368, 51760]);

    // Check that the command fails if there is a maxAwaitTimeMS field but no topologyVersion.
    assert.commandFailedWithCode(db.runCommand({[cmd]: 1, maxAwaitTimeMS: 0}), [31368, 51760]);

    // Check that the command fails if there is a valid topologyVersion but invalid maxAwaitTimeMS
    // type.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: "stringMaxAwaitTimeMS"
    }),
                                 ErrorCodes.TypeMismatch);

    // Check that the command fails if there is a valid topologyVersion but negative maxAwaitTimeMS.
    assert.commandFailedWithCode(db.runCommand({
        [cmd]: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: -1,
    }),
                                 [31373, 51759]);
}

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");
runTest(conn.getDB("admin"), "hello");
runTest(conn.getDB("admin"), "isMaster");
runTest(conn.getDB("admin"), "ismaster");
MongoRunner.stopMongod(conn);

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();
runTest(replTest.getPrimary().getDB("admin"), "hello");
runTest(replTest.getPrimary().getDB("admin"), "isMaster");
runTest(replTest.getPrimary().getDB("admin"), "ismaster");
replTest.stopSet();

const st = new ShardingTest({mongos: 1, shards: [{nodes: 1}], config: 1});
runTest(st.s.getDB("admin"), "hello");
runTest(st.s.getDB("admin"), "isMaster");
runTest(st.s.getDB("admin"), "ismaster");
st.stop();
})();
