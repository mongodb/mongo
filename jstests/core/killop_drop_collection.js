/**
 * A killOp command issued against a collection drop should not interfere with the drop and allow it
 * to complete. Interrupting a collection drop could leave the database in an inconsistent state.
 * This test confirms that killOp won't interrupt a collection drop, and that the drop occurs
 * successfully.
 *
 * @tags: [
 *   assumes_superuser_permissions,
 *   # Uses index building in background
 *   requires_background_index,
 * ]
 */
(function() {
    "use strict";

    var collectionName = "killop_drop";
    let collection = db.getCollection(collectionName);
    collection.drop();
    for (let i = 0; i < 1000; i++) {
        assert.writeOK(collection.insert({x: i}));
    }
    assert.writeOK(collection.createIndex({x: 1}, {background: true}));

    // Attempt to fsyncLock the database, aborting early if the storage engine doesn't support it.
    const storageEngine = jsTest.options().storageEngine;
    let fsyncRes = db.fsyncLock();
    if (!fsyncRes.ok) {
        assert.commandFailedWithCode(fsyncRes, ErrorCodes.CommandNotSupported);
        jsTest.log("Skipping test on storage engine " + storageEngine +
                   ", which does not support fsyncLock.");
        return;
    }

    // Kick off a drop on the collection.
    const useDefaultPort = null;
    const noConnect = false;
    // The drop will occasionally, and legitimately be interrupted by killOp (and not succeed).
    let awaitDropCommand = startParallelShell(function() {
        let res = db.getSiblingDB("test").runCommand({drop: "killop_drop"});
        let collectionFound = db.getCollectionNames().includes("killop_drop");
        if (res.ok == 1) {
            // Ensure that the collection has been dropped.
            assert(
                !collectionFound,
                "Expected collection to not appear in listCollections output after being dropped");
        } else {
            // Ensure that the collection hasn't been dropped.
            assert(collectionFound,
                   "Expected collection to appear in listCollections output after drop failed");
        }
    }, useDefaultPort, noConnect);

    // Wait for the drop operation to appear in the db.currentOp() output.
    let dropCommandOpId = null;
    assert.soon(function() {
        let dropOpsInProgress = db.currentOp().inprog.filter(
            op => op.command && op.command.drop === collection.getName());
        if (dropOpsInProgress.length > 0) {
            dropCommandOpId = dropOpsInProgress[0].opid;
        }
        return dropCommandOpId;
    });

    // Issue a killOp for the drop command, then unlock the server. We expect that the drop
    // operation was *not* killed, and that the collection was dropped successfully.
    assert.commandWorked(db.killOp(dropCommandOpId));
    let unlockRes = assert.commandWorked(db.fsyncUnlock());
    assert.eq(0,
              unlockRes.lockCount,
              "Expected the number of fsyncLocks to be zero after issuing fsyncUnlock");

    awaitDropCommand();

}());
