/**
 * Tests the maxAwaitTimeMS and topologyVersion parameters of the hello command, and its aliases,
 * isMaster and ismaster.
 * @tags: [requires_replication]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// ErrorCodes
const kIDLParserComparisonError = 51024;

// runTest takes in the hello command or its aliases, isMaster and ismaster.
function runTest(db, cmd, logFailpoint, failpointName) {
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
    assert.commandWorked(db.runCommand({[cmd]: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 0}));

    // Ensure the command waits for at least maxAwaitTimeMS before returning, and doesn't appear in
    // slow query log even if it takes many seconds.
    assert.commandWorked(db.adminCommand({clearLog: "global"}));
    let now = new Date();
    jsTestLog(`Running slow ${cmd}`);

    // Get the slow query log failpoint for the command, to know the current timesEntered before
    // the command runs.
    const timesEnteredBeforeRunningCommand = configureFailPoint(db, logFailpoint).timesEntered;

    assert.commandWorked(db.runCommand({[cmd]: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 20000}));
    let commandDuration = new Date() - now;
    // Allow for some clock imprecision between the server and the jstest.
    assert.gte(
        commandDuration,
        10000,
        cmd + ` command should have taken at least 10000ms, but completed in ${commandDuration}ms`,
    );

    // Get the slow query log failpoint for the command, to make sure that it didn't get hit during
    // the command run by checking that timesEntered is the same as before.
    const timesEnteredAfterRunningCommand = configureFailPoint(db, logFailpoint).timesEntered;
    assert(timesEnteredBeforeRunningCommand == timesEnteredAfterRunningCommand);

    // Check that the command appears in the slow query log if it's unexpectedly slow.
    function runHelloCommand(cmd, topologyVersionField) {
        assert.commandWorked(db.runCommand({[cmd]: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 1}));
        jsTestLog(`${cmd} completed in parallel shell`);
    }

    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    // Use a skip of 1, since the parallel shell runs hello when it starts.
    const helloFailpoint = configureFailPoint(db, failpointName, {}, {skip: 1});
    const logFailPoint = configureFailPoint(db, logFailpoint);
    const awaitHello = startParallelShell(funWithArgs(runHelloCommand, cmd, topologyVersionField), db.getMongo().port);
    helloFailpoint.wait();
    sleep(1000); // Make the command hang for a second in the parallel shell.
    helloFailpoint.off();

    // Wait for the parallel shell to finish.
    awaitHello();

    // Wait for the command to be logged.
    logFailPoint.wait();

    // Check that when a different processId is given, the server responds immediately.
    now = new Date();
    assert.commandWorked(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {processId: ObjectId(), counter: topologyVersionField.counter},
            maxAwaitTimeMS: 2000,
        }),
    );
    commandDuration = new Date() - now;
    assert.lt(
        commandDuration,
        1000,
        cmd + ` command should have taken at most 1000ms, but completed in ${commandDuration}ms`,
    );

    // Check that when a different processId is given, a higher counter is allowed.
    assert.commandWorked(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {processId: ObjectId(), counter: NumberLong(topologyVersionField.counter + 1)},
            maxAwaitTimeMS: 0,
        }),
    );

    // Check that when the same processId is given, a higher counter is not allowed.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {
                processId: topologyVersionField.processId,
                counter: NumberLong(topologyVersionField.counter + 1),
            },
            maxAwaitTimeMS: 0,
        }),
        [31382, 51761, 51764],
    );

    // Check that passing a topologyVersion not of type object fails.
    assert.commandFailedWithCode(
        db.runCommand({[cmd]: 1, topologyVersion: "topology_version_string", maxAwaitTimeMS: 0}),
        ErrorCodes.TypeMismatch,
    );

    // Check that a topologyVersion with an invalid processId and valid counter fails.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {processId: "pid1", counter: topologyVersionField.counter},
            maxAwaitTimeMS: 0,
        }),
        ErrorCodes.TypeMismatch,
    );

    // Check that a topologyVersion with a valid processId and invalid counter fails.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {processId: topologyVersionField.processId, counter: 0},
            maxAwaitTimeMS: 0,
        }),
        ErrorCodes.TypeMismatch,
    );

    // Check that a topologyVersion with a valid processId but missing counter fails.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {processId: topologyVersionField.processId},
            maxAwaitTimeMS: 0,
        }),
        ErrorCodes.IDLFailedToParse,
    );

    // Check that a topologyVersion with a missing processId and valid counter fails.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {counter: topologyVersionField.counter},
            maxAwaitTimeMS: 0,
        }),
        ErrorCodes.IDLFailedToParse,
    );

    // Check that a topologyVersion with a valid processId and negative counter fails.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {processId: topologyVersionField.processId, counter: NumberLong("-1")},
            maxAwaitTimeMS: 0,
        }),
        [31372, 51758],
    );

    // Check that the command fails if there is an extra field in its topologyVersion.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: {
                processId: topologyVersionField.processId,
                counter: topologyVersionField.counter,
                randomField: "I should cause an error",
            },
            maxAwaitTimeMS: 0,
        }),
        ErrorCodes.IDLUnknownField,
    );

    // A client following the awaitable hello/isMaster protocol must include topologyVersion in
    // their request if and only if they include maxAwaitTimeMS. Check that the command fails if
    // there is a topologyVersion but no maxAwaitTimeMS field.
    assert.commandFailedWithCode(db.runCommand({[cmd]: 1, topologyVersion: topologyVersionField}), [31368, 51760]);

    // Check that the command fails if there is a maxAwaitTimeMS field but no topologyVersion.
    assert.commandFailedWithCode(db.runCommand({[cmd]: 1, maxAwaitTimeMS: 0}), [31368, 51760]);

    // Check that the command fails if there is a valid topologyVersion but invalid maxAwaitTimeMS
    // type.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: topologyVersionField,
            maxAwaitTimeMS: "stringMaxAwaitTimeMS",
        }),
        ErrorCodes.TypeMismatch,
    );

    // Check that the command fails if there is a valid topologyVersion but negative maxAwaitTimeMS.
    assert.commandFailedWithCode(
        db.runCommand({
            [cmd]: 1,
            topologyVersion: topologyVersionField,
            maxAwaitTimeMS: -1,
        }),
        [31373, 51759, ErrorCodes.BadValue, kIDLParserComparisonError],
    ); // getting BadValue when binary is > 7.1, else kIDLParserComparisonError
}

// Set command log verbosity to 0 to avoid logging *all* commands in the "slow query" log.
const conn = MongoRunner.runMongod({setParameter: {logComponentVerbosity: tojson({command: 0})}});
assert.neq(null, conn, "mongod was unable to start up");
// Set slowMs threshold to 500ms.
assert.commandWorked(conn.getDB("admin").setProfilingLevel(1, 500));
runTest(conn.getDB("admin"), "hello", "waitForHelloCommandLogged", "shardWaitInHello");
runTest(conn.getDB("admin"), "isMaster", "waitForIsMasterCommandLogged", "shardWaitInHello");
runTest(conn.getDB("admin"), "ismaster", "waitForIsMasterCommandLogged", "shardWaitInHello");
MongoRunner.stopMongod(conn);

const replTest = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({command: 0})}},
});
replTest.startSet();
replTest.initiate();
// Set slowMs threshold to 500ms.
assert.commandWorked(replTest.getPrimary().getDB("admin").setProfilingLevel(1, 500));
runTest(replTest.getPrimary().getDB("admin"), "hello", "waitForHelloCommandLogged", "shardWaitInHello");
runTest(replTest.getPrimary().getDB("admin"), "isMaster", "waitForIsMasterCommandLogged", "shardWaitInHello");
runTest(replTest.getPrimary().getDB("admin"), "ismaster", "waitForIsMasterCommandLogged", "shardWaitInHello");
replTest.stopSet();

const st = new ShardingTest({
    mongos: 1,
    shards: [{nodes: 1}],
    config: 1,
    other: {mongosOptions: {setParameter: {logComponentVerbosity: tojson({command: 0})}}},
});
// Set slowMs threshold to 500ms.
assert.commandWorked(st.s.getDB("admin").setProfilingLevel(0, 500));
runTest(st.s.getDB("admin"), "hello", "waitForHelloCommandLogged", "routerWaitInHello");
runTest(st.s.getDB("admin"), "isMaster", "waitForIsMasterCommandLogged", "routerWaitInHello");
runTest(st.s.getDB("admin"), "ismaster", "waitForIsMasterCommandLogged", "routerWaitInHello");
st.stop();
