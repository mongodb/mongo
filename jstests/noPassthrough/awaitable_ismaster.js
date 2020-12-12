/**
 * Tests the maxAwaitTimeMS and topologyVersion parameters of the isMaster command.
 * @tags: [requires_replication]
 */
(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

function runTest(db) {
    // Check isMaster response contains a topologyVersion even if maxAwaitTimeMS and topologyVersion
    // are not included in the request.
    const res = assert.commandWorked(db.runCommand({isMaster: 1}));
    assert(res.hasOwnProperty("topologyVersion"), tojson(res));

    const topologyVersionField = res.topologyVersion;
    assert(topologyVersionField.hasOwnProperty("processId"), tojson(topologyVersionField));
    assert(topologyVersionField.hasOwnProperty("counter"), tojson(topologyVersionField));

    // Check that isMaster succeeds when passed a valid topologyVersion and maxAwaitTimeMS. In this
    // case, use the topologyVersion from the previous isMaster response. The topologyVersion field
    // is expected to be of the form {processId: <ObjectId>, counter: <long>}.
    assert.commandWorked(
        db.runCommand({isMaster: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 0}));

    // Ensure the command waits for at least maxAwaitTimeMS before returning, and doesn't appear in
    // slow query log even if it takes many seconds.
    assert.commandWorked(db.adminCommand({clearLog: 'global'}));
    let now = new Date();
    jsTestLog(`Running slow isMaster`);
    assert.commandWorked(
        db.runCommand({isMaster: 1, topologyVersion: topologyVersionField, maxAwaitTimeMS: 20000}));
    let isMasterDuration = new Date() - now;
    // Allow for some clock imprecision between the server and the jstest.
    assert.gte(
        isMasterDuration,
        10000,
        `isMaster should have taken at least 10000ms, but completed in ${isMasterDuration}ms`);

    assert(!checkLog.checkContainsOnceJson(db.getMongo(), 51803, {
        'command': function(obj) {
            return obj.hasOwnProperty('isMaster');
        }
    }));

    // Check that when a different processId is given, the server responds immediately.
    now = new Date();
    assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion: {processId: ObjectId(), counter: topologyVersionField.counter},
        maxAwaitTimeMS: 2000
    }));
    isMasterDuration = new Date() - now;
    assert.lt(isMasterDuration,
              1000,
              `isMaster should have taken at most 1000ms, but completed in ${isMasterDuration}ms`);

    // Check that when a different processId is given, a higher counter is allowed.
    assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion:
            {processId: ObjectId(), counter: NumberLong(topologyVersionField.counter + 1)},
        maxAwaitTimeMS: 0
    }));

    // Check that when the same processId is given, a higher counter is not allowed.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: {
            processId: topologyVersionField.processId,
            counter: NumberLong(topologyVersionField.counter + 1)
        },
        maxAwaitTimeMS: 0
    }),
                                 [31382, 51761, 51764]);

    // Check that passing a topologyVersion not of type object fails.
    assert.commandFailedWithCode(
        db.runCommand({isMaster: 1, topologyVersion: "topology_version_string", maxAwaitTimeMS: 0}),
        10065);

    // Check that a topologyVersion with an invalid processId and valid counter fails.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: {processId: "pid1", counter: topologyVersionField.counter},
        maxAwaitTimeMS: 0
    }),
                                 ErrorCodes.TypeMismatch);

    // Check that a topologyVersion with a valid processId and invalid counter fails.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: {processId: topologyVersionField.processId, counter: 0},
        maxAwaitTimeMS: 0
    }),
                                 ErrorCodes.TypeMismatch);

    // Check that a topologyVersion with a valid processId but missing counter fails.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: {processId: topologyVersionField.processId},
        maxAwaitTimeMS: 0
    }),
                                 40414);

    // Check that a topologyVersion with a missing processId and valid counter fails.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: {counter: topologyVersionField.counter},
        maxAwaitTimeMS: 0
    }),
                                 40414);

    // Check that a topologyVersion with a valid processId and negative counter fails.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: {processId: topologyVersionField.processId, counter: NumberLong("-1")},
        maxAwaitTimeMS: 0
    }),
                                 [31372, 51758]);

    // Check that isMaster fails if there is an extra field in its topologyVersion.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: {
            processId: topologyVersionField.processId,
            counter: topologyVersionField.counter,
            randomField: "I should cause an error"
        },
        maxAwaitTimeMS: 0
    }),
                                 40415);

    // A client following the awaitable isMaster protocol must include topologyVersion in their
    // request if and only if they include maxAwaitTimeMS. Check that isMaster fails if there is a
    // topologyVersion but no maxAwaitTimeMS field.
    assert.commandFailedWithCode(
        db.runCommand({isMaster: 1, topologyVersion: topologyVersionField}), [31368, 51760]);

    // Check that isMaster fails if there is a maxAwaitTimeMS field but no topologyVersion.
    assert.commandFailedWithCode(db.runCommand({isMaster: 1, maxAwaitTimeMS: 0}), [31368, 51760]);

    // Check that isMaster fails if there is a valid topologyVersion but invalid maxAwaitTimeMS
    // type.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: "stringMaxAwaitTimeMS"
    }),
                                 ErrorCodes.TypeMismatch);

    // Check that isMaster fails if there is a valid topologyVersion but negative maxAwaitTimeMS.
    assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: -1,
    }),
                                 [31373, 51759]);
}

// Set command log verbosity to 0 to avoid logging *all* commands in the "slow query" log.
const conn = MongoRunner.runMongod({setParameter: {logComponentVerbosity: tojson({command: 0})}});
assert.neq(null, conn, "mongod was unable to start up");
runTest(conn.getDB("admin"));
MongoRunner.stopMongod(conn);

const replTest = new ReplSetTest(
    {nodes: 1, nodeOptions: {setParameter: {logComponentVerbosity: tojson({command: 0})}}});
replTest.startSet();
replTest.initiate();
runTest(replTest.getPrimary().getDB("admin"));
replTest.stopSet();

const st = new ShardingTest({
    mongos: 1,
    shards: [{nodes: 1}],
    config: 1,
    other: {mongosOptions: {setParameter: {logComponentVerbosity: tojson({command: 0})}}}
});
runTest(st.s.getDB("admin"));
st.stop();
})();
