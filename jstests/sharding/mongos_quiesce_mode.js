/**
 * Tests the behavior of quiesce mode on mongos, which is entered during shutdown.
 * During quiesce mode, existing operations are allowed to continue and new operations are
 * accepted. However, isMaster requests return a ShutdownInProgress error, so that clients can
 * begin re-routing operations.
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

const st = new ShardingTest({shards: [{nodes: 1}], mongos: 1});
const mongos = st.s;
const mongodPrimary = st.rs0.getPrimary();

const dbName = "test";
const collName = "coll";
const mongosDB = mongos.getDB(dbName);
assert.commandWorked(mongosDB.coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));

function checkTopologyVersion(res, topologyVersionField) {
    assert(res.hasOwnProperty("topologyVersion"), res);
    assert.eq(res.topologyVersion.counter, topologyVersionField.counter + 1);
}

function runAwaitableIsMaster(topologyVersionField) {
    let res = assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }),
                                           ErrorCodes.ShutdownInProgress);

    assert(res.hasOwnProperty("topologyVersion"), res);
    assert.eq(res.topologyVersion.counter, topologyVersionField.counter + 1);
}

function runFind() {
    assert.eq(4, db.getSiblingDB("test").coll.find().itcount());
}

function runInsert() {
    assert.commandWorked(db.getSiblingDB("test").coll.insert({_id: 4}));
}

jsTestLog("Create a cursor via mongos.");
let res = assert.commandWorked(mongosDB.runCommand({find: collName, batchSize: 2}));
assert.eq(2, res.cursor.firstBatch.length, res);
let cursorId = res.cursor.id;

jsTestLog("Create a hanging read operation via mongos.");
let findCmdFailPoint = configureFailPoint(mongos, "waitInFindBeforeMakingBatch");
let findCmd = startParallelShell(runFind, mongos.port);
findCmdFailPoint.wait();

// Hanging the write operation on mongod should be fine since mongos will not return to
// the client until it finishes.
jsTestLog("Create a hanging write operation via mongos.");
let insertCmdFailPoint = configureFailPoint(mongodPrimary, "hangAfterCollectionInserts");
let insertCmd = startParallelShell(runInsert, mongodPrimary.port);
insertCmdFailPoint.wait();

jsTestLog("Create a hanging isMaster via mongos.");
res = assert.commandWorked(mongos.adminCommand({isMaster: 1}));
assert(res.hasOwnProperty("topologyVersion"), res);
let topologyVersionField = res.topologyVersion;
let isMasterFailPoint = configureFailPoint(mongos, "waitForIsMasterResponse");
let isMaster =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionField), mongos.port);
isMasterFailPoint.wait();
assert.eq(1, mongos.getDB("admin").serverStatus().connections.awaitingTopologyChanges);

jsTestLog("Transition mongos to quiesce mode.");
let quiesceModeFailPoint = configureFailPoint(mongos, "hangDuringQuiesceMode");
// We must skip validation due to the failpoint that hangs find commands.
st.stopMongos(0 /* mongos index */, undefined /* opts */, {waitpid: false});
quiesceModeFailPoint.wait();

jsTestLog("The waiting isMaster returns a ShutdownInProgress error.");
isMaster();

jsTestLog("New isMaster command returns a ShutdownInProgress error.");
checkTopologyVersion(
    assert.commandFailedWithCode(mongos.adminCommand({isMaster: 1}), ErrorCodes.ShutdownInProgress),
    topologyVersionField);

// Test operation behavior during quiesce mode.
jsTestLog("The running read operation is allowed to finish.");
findCmdFailPoint.off();
findCmd();

jsTestLog("getMores on existing cursors are allowed.");
res = assert.commandWorked(mongosDB.runCommand({getMore: cursorId, collection: collName}));
assert.eq(2, res.cursor.nextBatch.length, res);

jsTestLog("The running write operation is allowed to finish.");
insertCmdFailPoint.off();
insertCmd();

jsTestLog("New reads are allowed.");
assert.eq(5, mongosDB.coll.find().itcount());

jsTestLog("New writes are allowed.");
assert.commandWorked(mongosDB.coll.insert({_id: 5}));

jsTestLog("Let shutdown progress to start killing operations.");
let pauseWhileKillingOperationsFailPoint =
    configureFailPoint(mongos, "pauseWhileKillingOperationsAtShutdown");

// Exit quiesce mode so we can hit the pauseWhileKillingOperationsFailPoint failpoint.
quiesceModeFailPoint.off();

// This throws because the configureFailPoint command is killed by the shutdown.
try {
    pauseWhileKillingOperationsFailPoint.wait();
} catch (e) {
    if (e.code === ErrorCodes.InterruptedAtShutdown) {
        jsTestLog(
            "Ignoring InterruptedAtShutdown error because configureFailPoint is killed by shutdown");
    } else {
        throw e;
    }
}

jsTestLog("Operations fail with a shutdown error and append the topologyVersion.");
checkTopologyVersion(assert.commandFailedWithCode(mongosDB.runCommand({find: collName}),
                                                  ErrorCodes.InterruptedAtShutdown),
                     topologyVersionField);
// Restart mongos.
st.restartMongos(0);

st.stop();
})();
