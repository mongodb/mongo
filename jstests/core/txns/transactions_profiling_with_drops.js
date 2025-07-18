// Tests that locks acquisitions for profiling in a transaction have a 0-second timeout.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession, profile.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_parallel_shell,
//   requires_profiling,
//   # The config fuzzer with a very low level of concurrency is exhausting the write tickets.
//   does_not_support_config_fuzzer,
//   # Uses $where
//   requires_scripting
// ]

import {
    profilerHasSingleMatchingEntryOrThrow,
    profilerHasZeroMatchingEntriesOrThrow,
} from "jstests/libs/profiler.js";
import {waitForCommand} from "jstests/libs/wait_for_command.js";

const dbName = "test";
const collName = "transactions_profiling_with_drops";
const adminDB = db.getSiblingDB("admin");
const testDB = db.getSiblingDB(dbName);
const session = db.getMongo().startSession({causalConsistency: false});
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

sessionDb.runCommand({dropDatabase: 1, writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.insert({_id: "doc"}, {w: "majority"}));
// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(sessionDb.runCommand(
    {profile: 1, filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

jsTest.log("Test read profiling with operation holding database X lock.");

jsTest.log("Start transaction.");
session.startTransaction();

jsTest.log("Run a slow read. Profiling in the transaction should succeed.");
assert.sameMembers(
    [{_id: "doc"}],
    sessionColl.find({$where: "sleep(1000); return true;"}).comment("read success").toArray());
profilerHasSingleMatchingEntryOrThrow(
    {profileDB: testDB, filter: {"command.comment": "read success"}});

// Lock 'test' database in X mode.
let lockShell = startParallelShell(function() {
    assert.commandFailed(db.adminCommand({
        sleep: 1,
        secs: 300,
        lock: "w",
        lockTarget: "test",
        $comment: "transaction_profiling_with_drops lock sleep"
    }));
});

// Wait for sleep to appear in currentOp
let opId = waitForCommand(
    "sleepCmd",
    op => (op["ns"] == "admin.$cmd" &&
           op["command"]["$comment"] == "transaction_profiling_with_drops lock sleep"),
    testDB);

jsTest.log("Run a slow read. Profiling in the transaction should fail.");
assert.sameMembers(
    [{_id: "doc"}],
    sessionColl.find({$where: "sleep(1000); return true;"}).comment("read failure").toArray());
assert.commandWorked(session.commitTransaction_forTesting());

assert.commandWorked(testDB.killOp(opId));
lockShell();

profilerHasZeroMatchingEntriesOrThrow(
    {profileDB: testDB, filter: {"command.comment": "read failure"}});

jsTest.log("Test write profiling with operation holding database X lock.");

jsTest.log("Start transaction.");
session.startTransaction();

jsTest.log("Run a slow write. Profiling in the transaction should succeed.");
assert.commandWorked(sessionColl.update(
    {$where: "sleep(1000); return true;"}, {$inc: {good: 1}}, {collation: {locale: "en"}}));
profilerHasSingleMatchingEntryOrThrow(
    {profileDB: testDB, filter: {"command.collation": {locale: "en"}}});

// Lock 'test' database in X mode.
lockShell = startParallelShell(function() {
    assert.commandFailed(db.getSiblingDB("test").adminCommand(
        {sleep: 1, secs: 300, lock: "w", lockTarget: "test", $comment: "lock sleep"}));
});

// Wait for sleep to appear in currentOp
opId = waitForCommand("sleepCmd",
                      op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "lock sleep"),
                      testDB);

jsTest.log("Run a slow write. Profiling in the transaction should still succeed " +
           "since the transaction already has an IX DB lock.");
assert.commandWorked(sessionColl.update(
    {$where: "sleep(1000); return true;"}, {$inc: {good: 1}}, {collation: {locale: "fr"}}));
assert.commandWorked(session.commitTransaction_forTesting());

assert.commandWorked(testDB.killOp(opId));
lockShell();

profilerHasSingleMatchingEntryOrThrow(
    {profileDB: testDB, filter: {"command.collation": {locale: "fr"}}});

jsTest.log("Both writes should succeed");
assert.docEq({_id: "doc", good: 2}, sessionColl.findOne());

assert.commandWorked(sessionDb.runCommand({profile: 0}));
session.endSession();
