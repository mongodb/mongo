/**
 * A killOp command issued against a collection drop should not interfere with the drop and allow it
 * to complete. Interrupting a collection drop could leave the database in an inconsistent state.
 * This test confirms that killOp won't interrupt a collection drop, and that the drop occurs
 * successfully.
 */
(function() {
    "use strict";

    var collectionName = "killop_drop";
    var collection = db.getCollection(collectionName);
    collection.drop();
    assert.writeOK(collection.insert({x: 1}));

    // Attempt to fsyncLock the database, aborting early if the storage engine doesn't support it.
    var storageEngine = jsTest.options().storageEngine;
    var fsyncRes = db.fsyncLock();
    if (!fsyncRes.ok) {
        assert.commandFailedWithCode(fsyncRes, ErrorCodes.CommandNotSupported);
        jsTest.log("Skipping test on storage engine " + storageEngine +
                   ", which does not support fsyncLock.");
        return;
    }

    // Kick off a drop on the collection.
    var useDefaultPort = null;
    var noConnect = false;
    var awaitDropCommand = startParallelShell(function() {
        assert.commandWorked(db.getSiblingDB("test").runCommand({drop: "killop_drop"}));
    }, useDefaultPort, noConnect);

    // Wait for the drop operation to appear in the db.currentOp() output.
    var dropCommandOpId = null;
    assert.soon(function() {
        var dropOpsInProgress = db.currentOp().inprog.filter(function(op) {
            return op.query && op.query.drop === collection.getName();
        });
        if (dropOpsInProgress.length > 0) {
            dropCommandOpId = dropOpsInProgress[0].opid;
        }
        return dropCommandOpId;
    });

    // Issue a killOp for the drop command, then unlock the server. We expect that the drop
    // operation was *not* killed, and that the collection was dropped successfully.
    assert.commandWorked(db.killOp(dropCommandOpId));
    assert.commandWorked(db.fsyncUnlock());
    awaitDropCommand();

    // Ensure that the collection has been dropped.
    assert.eq(-1,
              db.getCollectionNames().indexOf(collectionName),
              "Expected collection to not appear in listCollections output after being dropped");
}());
