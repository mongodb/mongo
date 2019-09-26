/**
 * Tests that index builds don't block on operations that conflict because they are in a prepared
 * state.
 *
 * @tags: [
 *   requires_document_locking,
 *   requires_replication,
 *   two_phase_index_builds_unsupported,
 *   uses_prepare_transaction,
 *   uses_transactions,
 *   ]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");  // for PrepareHelpers
load("jstests/noPassthrough/libs/index_build.js");  // for IndexBuildTest

const replSetTest = new ReplSetTest({
    name: "index_builds_ignore_prepare_conflicts",
    nodes: [
        {},
        {rsConfig: {priority: 0}},
    ],
});
replSetTest.startSet();
replSetTest.initiate();

const primary = replSetTest.getPrimary();
const primaryDB = primary.getDB('test');

let numDocs = 10;
let setUp = function(coll) {
    coll.drop();
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({i: i});
    }
    assert.commandWorked(bulk.execute());
};

/**
 * Run a background index build, and depending on the provided node, 'conn', ensure that a
 * prepared update does not introduce prepare conflicts on the index builder.
 */
let runTest = function(conn) {
    const testDB = conn.getDB('test');

    const collName = 'index_builds_ignore_prepare_conflicts';
    const coll = primaryDB.getCollection(collName);
    setUp(coll);

    // Start and pause an index build.
    IndexBuildTest.pauseIndexBuilds(conn);
    const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});
    const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, collName, 'i_1');

    // This insert will block until the index build pauses and releases its exclusive lock.
    // This guarantees that the subsequent transaction can immediately acquire a lock and not
    // fail with a LockTimeout error.
    assert.commandWorked(coll.insert({i: numDocs++}));

    // Start a session and introduce a document that is in a prepared state, but should be
    // ignored by the index build, at least until the transaction commits.
    const session = primaryDB.getMongo().startSession();
    const sessionDB = session.getDatabase('test');
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update({i: 0}, {i: "prepared"}));
    // Use w:1 because the secondary will be unable to replicate the prepare while an index
    // build is running.
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});

    // Let the index build continue until just before it completes. Set the failpoint just
    // before the second drain, which would take lock that conflicts with the prepared
    // transaction and prevent the index build from completing entirely.
    const failPointName = "hangAfterIndexBuildFirstDrain";
    clearRawMongoProgramOutput();
    assert.commandWorked(conn.adminCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));

    // Unpause the index build from the first failpoint so that it can resume and pause at the
    // next failpoint.
    IndexBuildTest.resumeIndexBuilds(conn);
    assert.soon(() =>
                    rawMongoProgramOutput().indexOf("Hanging after index build first drain") >= 0);

    // Right before the index build completes, ensure no prepare conflicts were hit.
    IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId, (op) => {
        printjson(op);
        assert.eq(undefined, op.prepareReadConflicts);
    });

    // Because prepare uses w:1, ensure it is majority committed before committing the
    // transaction.
    PrepareHelpers.awaitMajorityCommitted(replSetTest, prepareTimestamp);

    // Commit the transaction before completing the index build, releasing locks which will
    // allow the index build to complete.
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    // Allow the index build to complete.
    assert.commandWorked(conn.adminCommand({configureFailPoint: failPointName, mode: "off"}));

    awaitBuild();
    IndexBuildTest.waitForIndexBuildToStop(testDB, collName, "i_1");

    assert.eq(numDocs, coll.count());
    assert.eq(numDocs, coll.find().itcount());
};

runTest(replSetTest.getPrimary());

replSetTest.stopSet();
})();
