/**
 * Tests that a change stream resume token captured before a primary stepdown remains usable
 * after the original primary returns to PRIMARY and a no-op write is performed to advance the
 * majority commit point past the resume token.
 *
 * Scenario:
 *   1. Open a change stream on the primary; perform writes; capture a resume token.
 *   2. Force the primary to step down, then step it back up.
 *   3. Perform a no-op write (appendOplogNote) on the freshly-elected primary so that the
 *      majority commit point advances past the captured resume token's cluster time.
 *   4. Open a new change stream with {resumeAfter: token}; assert it returns subsequent events.
 *
 * The no-op write is the load-bearing step: without it, the resume token can sit beyond the
 * majority commit point if no client traffic arrives after the election, which prevents the
 * resumed cursor from being satisfied.
 *
 * @tags: [
 *   requires_replication,
 *   uses_change_streams,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

const replTest = new ReplSetTest({name: jsTestName(), nodes: 2});
replTest.startSet();
replTest.initiate();

const dbName = jsTestName();
const collName = "test";

let primary = replTest.getPrimary();
let primaryDb = primary.getDB(dbName);
let coll = primaryDb[collName];

assert.commandWorked(coll.insert({_id: "seed"}));
replTest.awaitReplication();

// Open a change stream and capture a resume token after one event.
let csTest = new ChangeStreamTest(primaryDb);
let cursor = csTest.startWatchingChanges({
    collection: collName,
    pipeline: [{$changeStream: {}}],
});

assert.commandWorked(coll.insert({_id: 1}));
replTest.awaitReplication();

const firstEvent = csTest.getNextChanges(cursor, 1)[0];
assert.eq(firstEvent.operationType, "insert");
assert.eq(firstEvent.fullDocument._id, 1);
const resumeToken = firstEvent._id;
assert.neq(resumeToken, undefined, "Expected a resume token on the first event.");

// Force a stepdown, then step the original primary back up.
assert.commandWorked(primary.adminCommand({replSetStepDown: 30, force: true}));
replTest.awaitSecondaryNodes(null, [primary]);

assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
primary = replTest.stepUp(primary, {awaitReplicationBeforeStepUp: false});
primaryDb = primary.getDB(dbName);
coll = primaryDb[collName];

// No-op write: advance the majority commit point without producing a user-visible event.
// appendOplogNote writes an oplog entry of op:"n" that the change stream pipeline ignores,
// but it advances the majority commit point so the resumed cursor has something to anchor to.
assert.commandWorked(primary.getDB("admin").runCommand({
    appendOplogNote: 1,
    data: {msg: "resume-after-stepdown-noop", testName: jsTestName()},
}));
replTest.awaitReplication();

// Subsequent client write that the resumed stream should observe.
assert.commandWorked(coll.insert({_id: 2}));
replTest.awaitReplication();

// Open a fresh change stream resuming from the captured token.
csTest = new ChangeStreamTest(primaryDb);
const resumeCursor = csTest.startWatchingChanges({
    collection: collName,
    pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
});

csTest.assertNextChangesEqual({
    cursor: resumeCursor,
    expectedChanges: [
        {
            operationType: "insert",
            ns: {db: dbName, coll: collName},
            fullDocument: {_id: 2},
            documentKey: {_id: 2},
        },
    ],
});

csTest.cleanUp();
replTest.stopSet();
