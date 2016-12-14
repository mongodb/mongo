/**
 * A killOp command issued against a collection drop should not interfere with the drop and allow it
 * to complete. Interrupting a collection drop could leave the database in an inconsistent state.
 * This test confirms that killOp won't interrupt a collection drop, and that the drop occurs
 * successfully.
 */
(function() {
    "use strict";

    const collectionName = "killop_drop";
    let collection = db.getCollection(collectionName);
    collection.drop();
    assert.writeOK(collection.insert({x: 1}));

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
    let awaitDropCommand = startParallelShell(function() {
        assert.commandWorked(db.getSiblingDB("test").runCommand({drop: "killop_drop"}));
    }, useDefaultPort, noConnect);

    // Wait for the drop operation to appear in the db.currentOp() output.
    let dropCommandOpId = null;
    assert.soon(function() {
        let dropOpsInProgress =
            db.currentOp().inprog.filter(op => op.query && op.query.drop === collection.getName());
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

    // Ensure that the collection has been dropped.
    assert(!db.getCollectionNames().includes(collectionName),
           "Expected collection to not appear in listCollections output after being dropped");
}());
