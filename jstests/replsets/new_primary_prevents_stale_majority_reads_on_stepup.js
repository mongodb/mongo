/**
 * Test that after an election, the new primary blocks majority reads until it has committed its
 * first write in the new term, in order to prevent serving stale majority reads. See SERVER-53813
 * for details.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Test the case where step-up and committing the "new primary" entry succeed.
{
    const rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {setParameter: {logComponentVerbosity: tojson({'command': 2})}}});
    rst.startSet();
    rst.initiate();

    const oldPrimary = rst.getPrimary();
    const testDB = oldPrimary.getDB(jsTestName());
    const collectionName = "test";

    // Write a document that gets replicated to all nodes.
    assert.commandWorked(testDB.runCommand({insert: collectionName, documents: [{x: 1}]}));
    rst.awaitReplication();

    // Confirm the future primary has a majority committed snapshot to read from including that
    // write.
    const newPrimary = rst.getSecondaries()[0];
    assert.soon(() => {
        return newPrimary.getDB(jsTestName()).getCollection(collectionName).countDocuments({x: 1}, {
            readConcern: {level: "majority"}
        }) == 1;
    });

    // Set the disableSnapshotting failpoint to prevent the future primary from advancing its
    // current committed snapshot to reflect the "new primary" no-op entry.
    let failpoint = configureFailPoint(newPrimary, 'disableSnapshotting');

    // Trigger the secondary to step up.
    rst.stepUp(newPrimary);

    // Start a new read. The read should block until we have a committed snapshot available that
    // reflects the "new primary" entry.
    function parallelFunc(host, dbName, collectionName) {
        const res = assert.commandWorked(new Mongo(host).getDB(dbName).runCommand({
            find: collectionName,
            readConcern: {level: 'majority'},
            $readPreference: {mode: 'primary'},
        }));
        // Below, we do a write in order to trigger an update to the current committed snapshot.
        // This write will be unblocked once that snapshot becomes available, thus, this read
        // should always observe that write.
        // If we didn't see the correct number of documents here that would mean we didn't
        // correctly wait for the new primary entry to be majority committed and make it into
        // a snapshot.
        assert.eq(res.cursor.firstBatch.length, 2);
    }

    // Start the read and wait until we see it got to the point of waiting.
    const parallelResult = startParallelShell(
        funWithArgs(parallelFunc, newPrimary.host, jsTestName(), collectionName), newPrimary.port);
    assert.soon(() => {
        return checkLog.checkContainsOnce(newPrimary, 5381300);
    });

    // Disable the failpoint.
    failpoint.off();

    // Do a write. This is needed to trigger us to update the current committed snapshot.
    assert.commandWorked(
        newPrimary.getDB(jsTestName()).runCommand({insert: collectionName, documents: [{x: 2}]}));

    parallelResult();
    rst.stopSet();
}

// Test the case where the node steps up, accepts reads with primary read preference and rc:
// majority and manages to write an entry in the new term and create a replication waiter on it, but
// then ends up stepping down before that entry makes it into the majoritty committed snapshot and
// we can actually start serving majority reads.
{
    const rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {setParameter: {logComponentVerbosity: tojson({'command': 2})}}});
    rst.startSet();
    rst.initiate();

    const oldPrimary = rst.getPrimary();
    const testDB = oldPrimary.getDB(jsTestName());
    const collectionName = "test";

    // Write a document that gets replicated to all nodes.
    assert.commandWorked(testDB.runCommand({insert: collectionName, documents: [{x: 1}]}));
    rst.awaitReplication();

    // Confirm the future primary has a majority committed snapshot to read from including that
    // write.
    const newPrimary = rst.getSecondaries()[0];
    assert.soon(() => {
        return newPrimary.getDB(jsTestName()).getCollection(collectionName).countDocuments({x: 1}, {
            readConcern: {level: "majority"}
        }) == 1;
    });

    // Set the disableSnapshotting failpoint to prevent the future primary from advancing its
    // current committed snapshot to reflect the "new primary" no-op entry.
    let failpoint = configureFailPoint(newPrimary, 'disableSnapshotting');

    // Trigger the secondary to step up.
    rst.stepUp(newPrimary);

    // Start a new read. This should block waiting for the "new primary" entry to commit, and
    // eventually fail once we realize the node stepped down.
    function parallelFunc(host, dbName, collectionName) {
        assert.commandFailedWithCode(new Mongo(host).getDB(dbName).runCommand({
            find: collectionName,
            readConcern: {level: 'majority'},
            $readPreference: {mode: 'primary'},
        }),
                                     ErrorCodes.PrimarySteppedDown);
    }

    // Start the read and wait until we see it got to the point of waiting.
    const parallelResult = startParallelShell(
        funWithArgs(parallelFunc, newPrimary.host, jsTestName(), collectionName), newPrimary.port);
    assert.soon(() => {
        return checkLog.checkContainsOnce(newPrimary, 5381300);
    });

    // Tell the node to step down.
    assert.commandWorked(newPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs}));

    // Stepdown should lead us to fail the promise and unblock the read.
    parallelResult();
    rst.stopSet();
}
