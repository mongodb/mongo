/**
 * Tests that index-build collection scans ignore prepared updates, while side write drains release
 * their locks before waiting for prepared transactions.
 *
 * @tags: [
 *   requires_replication,
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const replSetTest = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {rsConfig: {priority: 0}}],
});
replSetTest.startSet();
replSetTest.initiate();

const primary = replSetTest.getPrimary();
const primaryDB = primary.getDB("test");

let numDocs = 10;
let setUp = function (coll) {
    coll.drop();
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({i: i});
    }
    assert.commandWorked(bulk.execute());
};

/**
 * Run a background index build, and depending on the provided node, 'conn', ensure that a
 * prepared update does not make the index builder wait while holding locks.
 */
let runTest = function (conn) {
    const testDB = conn.getDB("test");

    const collName = jsTestName();
    const coll = primaryDB.getCollection(collName);
    setUp(coll);

    // Start and pause an index build.
    IndexBuildTest.pauseIndexBuilds(conn);
    const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});
    const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, "i_1");

    // This insert will block until the index build pauses and releases its exclusive lock.
    // This guarantees that the subsequent transaction can immediately acquire a lock and not
    // fail with a LockTimeout error.
    assert.commandWorked(coll.insert({i: numDocs++}));

    // Start a session and prepare an update. The collection scan ignores it, but the side write
    // drain must release its locks before waiting for the transaction to resolve.
    const session = primaryDB.getMongo().startSession();
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({i: 0}, {i: "prepared"}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});
    // The resumable drain may read using a majority snapshot. Advance it through prepare before
    // unpausing, so the prepared side writes conflict rather than being timestamp-invisible.
    replSetTest.awaitLastOpCommitted(undefined, [conn]);

    // The unresolved transaction keeps the drain from completing.
    IndexBuildTest.resumeIndexBuilds(conn);
    assert.soon(() => {
        const [op] = testDB.currentOp({opid: opId, $all: true}).inprog;
        return op && op.prepareReadConflicts > 0 && Object.keys(op.locks || {}).length === 0;
    });

    // Scanning completed without a storage-level prepare wait. The drain also does not mark
    // the builder as storage-blocked, which would be unsafe during stepdown.
    IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId, (op) => {
        printjson(op);
        assert.gt(op.prepareReadConflicts, 0);
    });

    // Commit the transaction before completing the index build, releasing locks which will
    // allow the index build to complete.
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    awaitBuild();
    IndexBuildTest.waitForIndexBuildToStop(testDB, collName, "i_1");
    replSetTest.awaitReplication();

    assert.eq(numDocs, coll.count());
    assert.eq(numDocs, coll.find().itcount());
};

runTest(replSetTest.getPrimary());

replSetTest.stopSet();
