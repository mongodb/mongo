/**
 * Contains helper functions for testing dbCheck.
 */

// Apply function on all secondary nodes.
export const forEachSecondary = (replSet, f) => {
    for (let secondary of replSet.getSecondaries()) {
        f(secondary);
    }
};

// Apply function on primary and all secondary nodes.
export const forEachNode = (replSet, f) => {
    f(replSet.getPrimary());
    forEachSecondary(replSet, f);
};

// Clear local.system.healthlog.
export const clearHealthLog = (replSet) => {
    forEachNode(replSet, conn => conn.getDB("local").system.healthlog.drop());
};

export const dbCheckCompleted = (db) => {
    return db.currentOp().inprog.filter(x => x["desc"] == "dbCheck")[0] === undefined;
};

// Wait for dbCheck to complete (on both primaries and secondaries).  Fails an assertion if
// dbCheck takes longer than maxMs.
export const awaitDbCheckCompletion = (replSet, db, collName, maxKey, maxSize, maxCount) => {
    let start = Date.now();

    assert.soon(() => dbCheckCompleted(db), "dbCheck timed out");
    replSet.awaitSecondaryNodes();
    replSet.awaitReplication();

    forEachNode(replSet, function(node) {
        const healthlog = node.getDB('local').system.healthlog;
        assert.soon(function() {
            return (healthlog.find({"operation": "dbCheckStop"}).itcount() == 1);
        }, "dbCheck command didn't complete");
    });
};

// Clear health log and insert nDocs documents.
export const resetAndInsert = (replSet, db, collName, nDocs) => {
    db[collName].drop();
    clearHealthLog(replSet);

    assert.commandWorked(
        db[collName].insertMany([...Array(nDocs).keys()].map(x => ({a: x})), {ordered: false}));
    replSet.awaitReplication();
};

// Run dbCheck with given parameters and potentially wait for completion.
export const runDbCheck = (replSet,
                           db,
                           collName,
                           maxDocsPerBatch,
                           writeConcern = {
                               w: 'majority'
                           },
                           awaitCompletion = false) => {
    assert.commandWorked(db.runCommand({
        dbCheck: collName,
        maxDocsPerBatch: maxDocsPerBatch,
        batchWriteConcern: writeConcern,
    }));
    if (awaitCompletion) {
        awaitDbCheckCompletion(replSet, db);
    }
};
