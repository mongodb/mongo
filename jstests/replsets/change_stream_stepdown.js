/**
 * Test that a change stream on the primary node survives stepdown.
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const name = "change_stream_stepdown";
const replTest = new ReplSetTest({name: name, nodes: [{}, {}]});
replTest.startSet();
// Initiate with high election timeout to prevent any election races.
replTest.initiate();

function stepUp(replTest, conn) {
    assert.commandWorked(conn.adminCommand({replSetFreeze: 0}));
    // Steps up the node in conn and awaits for the stepped up node to become writable primary.
    return replTest.stepUp(conn, {awaitReplicationBeforeStepUp: false});
}

const dbName = name;
const collName = "change_stream_stepdown";
const changeStreamComment = collName + "_comment";

const primary = replTest.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb[collName];

// Open a change stream.
let res = primaryDb.runCommand({
    aggregate: collName,
    pipeline: [{$changeStream: {}}],
    cursor: {},
    comment: changeStreamComment,
    maxTimeMS: 5000,
});
assert.commandWorked(res);
let cursorId = res.cursor.id;

// Insert several documents on primary and let them majority commit.
assert.commandWorked(primaryColl.insert([{_id: 1}, {_id: 2}, {_id: 3}], {writeConcern: {w: "majority"}}));
replTest.awaitReplication();

jsTestLog("Testing that changestream survives stepdown between find and getmore");
// Step down.
assert.commandWorked(primaryDb.adminCommand({replSetStepDown: 60, force: true}));
replTest.awaitSecondaryNodes(null, [primary]);

// Receive the first change event.  This tests stepdown between find and getmore.
res = assert.commandWorked(primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
let changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 1});
assert.eq(changes[0]["operationType"], "insert");

jsTestLog("Testing that changestream survives step-up");
// Step back up and wait for primary.
stepUp(replTest, primary);

// Get the next one.  This tests that changestreams survives a step-up.
res = assert.commandWorked(primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 2});
assert.eq(changes[0]["operationType"], "insert");

jsTestLog("Testing that changestream survives stepdown between two getmores");
// Step down again.
assert.commandWorked(primaryDb.adminCommand({replSetStepDown: 60, force: true}));
replTest.awaitSecondaryNodes(null, [primary]);

// Get the next one.  This tests that changestreams survives a step down between getmores.
res = assert.commandWorked(primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 3});
assert.eq(changes[0]["operationType"], "insert");

// Step back up and wait for primary.
stepUp(replTest, primary);

jsTestLog("Testing that changestream waiting on old primary sees docs inserted on new primary");

replTest.awaitReplication(); // Ensure secondary is up to date and can win an election.

async function shellFn(dbName, collName, changeStreamComment, stepUpFn) {
    const {ReplSetTest} = await import("jstests/libs/replsettest.js");
    // Wait for the getMore to be in progress.
    const primary = db.getMongo();
    assert.soon(
        () =>
            primary
                .getDB("admin")
                .aggregate([
                    {"$currentOp": {}},
                    {
                        "$match": {
                            op: "getmore",
                            "cursor.originatingCommand.comment": changeStreamComment,
                        },
                    },
                ])
                .itcount() == 1,
    );

    const replTest = new ReplSetTest(primary.host);

    // Step down the old primary and wait for new primary.
    const newPrimary = stepUpFn(replTest, replTest.getSecondary());
    const newPrimaryDB = newPrimary.getDB(dbName);
    assert.neq(newPrimary, primary, "Primary didn't change.");

    jsTestLog("Inserting document on new primary");
    assert.commandWorked(newPrimaryDB[collName].insert({_id: 4}), {writeConcern: {w: "majority"}});
}
let waitForShell = startParallelShell(
    funWithArgs(shellFn, dbName, collName, changeStreamComment, stepUp),
    primary.port,
);

res = assert.commandWorked(
    primaryDb.runCommand({
        getMore: cursorId,
        collection: collName,
        batchSize: 1,
        maxTimeMS: ReplSetTest.kDefaultTimeoutMS,
    }),
);
changes = res.cursor.nextBatch;
assert.eq(changes.length, 1);
assert.eq(changes[0]["fullDocument"], {_id: 4});
assert.eq(changes[0]["operationType"], "insert");

waitForShell();

replTest.stopSet();
