/**
 * Tests the expected point-in-time lookup behaviour when instantiating collections using no shared
 * state.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     requires_fcv_70,
 * ]
 */
(function() {
"use strict";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set the history window to 1 hour to prevent the oldest timestamp from advancing in
            // order for drop pending tables to stick around.
            minSnapshotHistoryWindowInSeconds: 60 * 60,
            logComponentVerbosity: tojson({storage: 1}),
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const db = primary.getDB(dbName);
const coll = db.getCollection(jsTestName());

const kNumDocs = 5;
for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

const createIndexTS = assert.commandWorked(coll.createIndex({x: 1})).operationTime;
jsTestLog("Create index timestamp: " + tojson(createIndexTS));

const dropTS = assert.commandWorked(db.runCommand({drop: jsTestName()})).operationTime;
jsTestLog("Drop collection timestamp: " + tojson(dropTS));

// Instantiate a new collection without any shared state.
assert(checkLog.checkContainsWithCountJson(db, 6825401, {}, 0));

// Test that we can perform a point-in-time read from a drop pending table using an index.
let res = assert.commandWorked(db.runCommand({
    find: jsTestName(),
    hint: {x: 1},
    readConcern: {level: "snapshot", atClusterTime: createIndexTS}
}));
assert.eq(kNumDocs, res.cursor.firstBatch.length);

// Instantiate a new collection without any shared state.
checkLog.containsJson(db, 6825401);

rst.stopSet();
})();
