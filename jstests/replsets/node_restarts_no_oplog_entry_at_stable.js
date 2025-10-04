/**
 * Test that we can restart a node that has an oplog hole open at the stable optime
 * when we kill it.
 *
 * This test utilizes a single-node replica set and restarts its lone node during execution. Because
 * single-node replica sets are initiated with the latest FCV, when this test restarts the
 * node it is possible on multiversion suites for an older binary to be used that is incompatible
 * with the latest FCV document. Therefore, this test is multiversion incompatible.
 * @tags: [
 *   requires_persistence,
 *   multiversion_incompatible
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = TestData.testName;
const dbName = testName;
const collName = "testcoll";
const replTest = new ReplSetTest({name: testName, nodes: 1, nodeOptions: {"syncdelay": 5}});
replTest.startSet();
replTest.initiate(Object.extend(replTest.getReplSetConfig(), {writeConcernMajorityJournalDefault: false}));

const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const nss = primaryColl.getFullName();
TestData.collectionName = collName;

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Turn on checkpoint logging.
assert.commandWorked(
    primary.adminCommand({"setParameter": 1, "logComponentVerbosity": {"storage": {"recovery": 2, "verbosity": 1}}}),
);
jsTestLog("Writing data before oplog hole to collection.");
assert.commandWorked(primaryColl.insert({_id: "a"}));
jsTest.log("Create the uncommitted write.");

const failPoint = configureFailPoint(primaryDB, "hangAfterCollectionInserts", {
    collectionNS: primaryColl.getFullName(),
    first_id: "b",
});

const db = primaryDB;
const joinHungWrite = startParallelShell(() => {
    assert.commandFailed(db.getSiblingDB(TestData.testName)[TestData.collectionName].insert({_id: "b"}));
    return 0;
}, primary.port);
failPoint.wait();

jsTestLog("Creating a small write to advance the last committed timestamp.");
assert.commandWorked(primaryColl.insert({_id: "small1", small: "a"}));

// Ignore earlier checkpoints
assert.commandWorked(primary.adminCommand({clearLog: "global"}));

jsTestLog("Waiting for a stable checkpoint.");
checkLog.containsJson(primary, 23986);
jsTestLog("Got a stable checkpoint. Restarting the node.");
replTest.stop(
    primary,
    9 /* KILL */,
    {skipValidation: true, allowedExitCode: MongoRunner.EXIT_SIGKILL},
    {forRestart: true},
);
replTest.start(0, undefined /* options */, true /* restart */, true /* waitForHealth */);
replTest.waitForState(primary, ReplSetTest.State.PRIMARY);

jsTestLog("Joining failed write");
joinHungWrite({checkExitStatus: false});

replTest.stopSet();
