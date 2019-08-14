/**
 * Ensures that the 'buildUUID' is present for in-progress indexes when using the 'listIndexes()'
 * command.
 * @tags: [requires_replication]
 */
(function() {
'use strict';

const dbName = "test";
const collName = "coll";

const firstIndexName = "first";
const secondIndexName = "second";

function addTestDocuments(db) {
    let size = 100;
    jsTest.log("Creating " + size + " test documents.");
    var bulk = db.getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i, j: i * i});
    }
    assert.commandWorked(bulk.execute());
}

let replSet = new ReplSetTest({name: "indexBuilds", nodes: 2});
let nodes = replSet.nodeList();

replSet.startSet({startClean: true});
replSet.initiate({
    _id: "indexBuilds",
    members: [
        {_id: 0, host: nodes[0]},
        {_id: 1, host: nodes[1], votes: 0, priority: 0},
    ]
});

let primary = replSet.getPrimary();
let primaryDB = primary.getDB(dbName);

let secondary = replSet.getSecondary();
let secondaryDB = secondary.getDB(dbName);

addTestDocuments(primaryDB);
replSet.awaitReplication();

// Build and finish the first index.
assert.commandWorked(primaryDB.runCommand(
    {createIndexes: collName, indexes: [{key: {i: 1}, name: firstIndexName, background: true}]}));
replSet.waitForAllIndexBuildsToFinish(dbName, collName);

// Start hanging index builds on the secondary.
assert.commandWorked(secondaryDB.adminCommand(
    {configureFailPoint: "hangAfterStartingIndexBuild", mode: "alwaysOn"}));

// Build and hang on the second index.
assert.commandWorked(primaryDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {j: 1}, name: secondIndexName, background: true}],
    writeConcern: {w: 2}
}));

// Check the listIndexes() output.
let res = secondaryDB.runCommand({listIndexes: collName, includeBuildUUIDs: true});

assert.commandWorked(res);
let indexes = res.cursor.firstBatch;
assert.eq(3, indexes.length);

jsTest.log(indexes);

assert.eq(indexes[0].name, "_id_");
assert.eq(indexes[1].name, "first");
assert.eq(indexes[2].spec.name, "second");
assert(indexes[2].hasOwnProperty("buildUUID"));

// Allow the secondary to finish the index build.
assert.commandWorked(
    secondaryDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuild", mode: "off"}));

replSet.stopSet();
}());
