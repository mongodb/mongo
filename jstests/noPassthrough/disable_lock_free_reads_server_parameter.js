/**
 * Tests the 'featureFlagLockFreeReads' startup setParameter.
 *
 * User set featureFlagLockFreeReads will be overridden to false (disabled) if:
 *   - with enableMajorityReadConcern=false
 * Otherwise, the default for featureFlagLockFreeReads is false.
 *
 * This test is not compatible with the special Lock Free Reads build variant because
 * disableLockFreeReads is overridden there.
 *
 * @tags: [
 *     # This test expects enableMajorityReadConcern to be on by default and cannot run in suites
 *     # that explicitly change that.
 *     requires_majority_read_concern,
 *     requires_replication,
 *     incompatible_with_lockfreereads, // This test is not compatible with special LFR builder
 * ]
 */

(function() {
"use strict";

load("jstests/libs/parallelTester.js");
load("jstests/libs/wait_for_command.js");

/**
 * Set a 'lockMode' collection lock on 'collectionNs' to be held for 1 hour. This will ensure that
 * the lock will not be released before desired. The operation can be killed later to release the
 * lock.
 *
 * 'sleepComment' adds a comment so that the operation is can be identified for waitForCommand().
 */
function sleepFunction(host, collectionNs, lockMode, sleepComment) {
    const mongo = new Mongo(host);
    // Set a MODE_IS collection lock to be held for 1 hour.
    // Holding this lock for 1 hour will trigger a test timeout.
    assert.commandFailedWithCode(mongo.adminCommand({
        sleep: 1,
        secs: 3600,
        lockTarget: collectionNs,
        lock: lockMode,
        $comment: sleepComment
    }),
                                 ErrorCodes.Interrupted);
}

function runReadAgainstLock(host, dbName, collName, expectingLFR) {
    // Set up a document to later try to read.
    const mongo = new Mongo(host);
    const db = mongo.getDB(dbName);
    const coll = db.getCollection(collName);
    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));

    const sleepComment = "Lock sleep";
    const sleepCommand =
        new Thread(sleepFunction, host, dbName + "." + collName, "w", sleepComment);
    sleepCommand.start();

    // Wait for the sleep command to start.
    const sleepID = waitForCommand(
        "sleepCmd",
        op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == sleepComment),
        mongo.getDB("admin"));

    try {
        const findResult = coll.find({});
        if (expectingLFR) {
            assert.eq(1, coll.find({}).itcount());
        } else {
            const failureTimeoutMS = 1 * 1000;
            assert.commandFailedWithCode(
                db.runCommand({find: collName, maxTimeMS: failureTimeoutMS}),
                ErrorCodes.MaxTimeMSExpired);
        }
    } finally {
        // Kill the sleep command in order to release the collection lock.
        assert.commandWorked(mongo.getDB("admin").killOp(sleepID));
        sleepCommand.join();
    }
}

const replSetName = 'disable_lock_free_reads_server_parameter';
const dbName = "TestDB";
const collName = "TestColl";

jsTest.log(
    "Starting server with featureFlagLockFreeReads=true in standalone mode: this should turn " +
    "on lock-free reads.");

let conn = MongoRunner.runMongod({setParameter: "featureFlagLockFreeReads=true"});
assert(conn);
runReadAgainstLock(conn.host, dbName, collName, true);
MongoRunner.stopMongod(conn);

jsTest.log("Starting server with featureFlagLockFreeReads=false in standalone mode: this is the " +
           "default and nothing should happen.");

conn = MongoRunner.runMongod({setParameter: "featureFlagLockFreeReads=false"});
assert(conn);
runReadAgainstLock(conn.host, dbName, collName, false);
MongoRunner.stopMongod(conn);

jsTest.log(
    "Starting server with featureFlagLockFreeReads=true and enableMajorityReadConcern=false: " +
    "this should override the setting to false.");

let rst = new ReplSetTest({
    name: replSetName,
    nodes: 1,
    nodeOptions: {enableMajorityReadConcern: "false", setParameter: "featureFlagLockFreeReads=true"}
});
rst.startSet();
rst.initiate();
runReadAgainstLock(rst.getPrimary().host, dbName, collName, false);
rst.stopSet();

jsTest.log("Starting server as a replica set member with featureFlagLockFreeReads=true: this " +
           "should turn on lock-free reads.");

rst = new ReplSetTest(
    {name: replSetName, nodes: 1, nodeOptions: {setParameter: "featureFlagLockFreeReads=true"}});
rst.startSet();
rst.initiate();
runReadAgainstLock(rst.getPrimary().host, dbName, collName, true);
rst.stopSet();

jsTest.log(
    "Starting server as a replica set member with featureFlagLockFreeReads=false: this is the " +
    "default and nothing should happen.");

rst = new ReplSetTest(
    {name: replSetName, nodes: 1, nodeOptions: {setParameter: "featureFlagLockFreeReads=false"}});
rst.startSet();
rst.initiate();
runReadAgainstLock(rst.getPrimary().host, dbName, collName, false);
rst.stopSet();
}());
