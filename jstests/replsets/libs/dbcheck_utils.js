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

// Wait for dbCheck to complete (on both primaries and secondaries).
export const awaitDbCheckCompletion = (replSet, db) => {
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
export const runDbCheck = (replSet, db, collName, parameters = {}, awaitCompletion = false) => {
    let dbCheckCommand = {dbCheck: collName};
    for (let parameter in parameters) {
        dbCheckCommand[parameter] = parameters[parameter];
    }
    assert.commandWorked(db.runCommand(dbCheckCommand));
    if (awaitCompletion) {
        awaitDbCheckCompletion(replSet, db);
    }
};

export const checkHealthlog = (healthlog, query, numExpected, timeout = 60 * 1000) => {
    let query_count;
    assert.soon(
        function() {
            query_count = healthlog.find(query).itcount();
            return query_count == numExpected;
        },
        `dbCheck command didn't complete, health log query returned ${
            query_count} entries, expected ${numExpected}: ` +
            query,
        timeout);
};
