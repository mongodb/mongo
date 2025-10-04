/**
 * Tests the behavior of quiesce mode on mongos, which is entered during shutdown.
 * During quiesce mode, existing operations are allowed to continue and new operations are
 * accepted. However, hello requests return a ShutdownInProgress error, so that clients can
 * begin re-routing operations.
 * @tags: [
 *   # This test requires shutting down mongos alone.
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: [{nodes: 1}], mongos: 1});
const mongos = st.s;
const mongodPrimary = st.rs0.getPrimary();

const oldMongos = MongoRunner.compareBinVersions(mongos.fullOptions.binVersion, "5.3") <= 0;

const dbName = "test";
const collName = "coll";
const mongosDB = mongos.getDB(dbName);
assert.commandWorked(mongosDB.coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));

function checkTopologyVersion(res, topologyVersionField) {
    assert(res.hasOwnProperty("topologyVersion"), res);
    assert.eq(res.topologyVersion.counter, topologyVersionField.counter + 1);
}

function checkRemainingQuiesceTime(res) {
    assert(res.hasOwnProperty("remainingQuiesceTimeMillis"), res);
}

function runAwaitableHello(topologyVersionField) {
    let res = assert.commandFailedWithCode(
        db.runCommand({
            hello: 1,
            topologyVersion: topologyVersionField,
            maxAwaitTimeMS: 99999999,
        }),
        ErrorCodes.ShutdownInProgress,
    );

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

jsTestLog("Create a hanging hello via mongos.");
res = assert.commandWorked(mongos.adminCommand({hello: 1}));
assert(res.hasOwnProperty("topologyVersion"), res);
let topologyVersionField = res.topologyVersion;
let helloFailPoint = configureFailPoint(mongos, oldMongos ? "waitForHelloResponse" : "waitForHelloResponseMongos");
let hello = startParallelShell(funWithArgs(runAwaitableHello, topologyVersionField), mongos.port);
helloFailPoint.wait();
assert.eq(1, mongos.getDB("admin").serverStatus().connections.awaitingTopologyChanges);

jsTestLog("Transition mongos to quiesce mode.");
let quiesceModeFailPoint = configureFailPoint(
    mongos,
    oldMongos ? "hangDuringQuiesceMode" : "hangDuringQuiesceModeMongos",
);
// We must skip validation due to the failpoint that hangs find commands.
st.stopMongos(0 /* mongos index */, undefined /* opts */, {waitpid: false});
quiesceModeFailPoint.wait();

jsTestLog("The waiting hello returns a ShutdownInProgress error.");
hello();

jsTestLog("New hello command returns a ShutdownInProgress error.");
res = assert.commandFailedWithCode(mongos.adminCommand({hello: 1}), ErrorCodes.ShutdownInProgress);

checkTopologyVersion(res, topologyVersionField);
checkRemainingQuiesceTime(res);

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
let pauseWhileKillingOperationsFailPoint = configureFailPoint(mongos, "pauseWhileKillingOperationsAtShutdown");

// Exit quiesce mode so we can hit the pauseWhileKillingOperationsFailPoint failpoint.
quiesceModeFailPoint.off();

// This throws because the configureFailPoint command is killed by the shutdown.
if (!pauseWhileKillingOperationsFailPoint.wait({expectedErrorCodes: [ErrorCodes.InterruptedAtShutdown]})) {
    jsTestLog("Ignoring InterruptedAtShutdown error because `waitForFailPoint` is killed by shutdown");
}

jsTestLog("Operations fail with a shutdown error and append the topologyVersion.");
checkTopologyVersion(
    assert.commandFailedWithCode(mongosDB.runCommand({find: collName}), ErrorCodes.InterruptedAtShutdown),
    topologyVersionField,
);
// Restart mongos.
st.restartMongos(0);

st.stop();
