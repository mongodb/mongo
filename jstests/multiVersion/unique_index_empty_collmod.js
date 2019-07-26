/**
 * Ensure that unique indexes are not updated on running an empty collMod command with FCV=4.0.
 * Creates a unique index, downgrades the FCV, and runs collMod on the collection.
 * @tags: [requires_replication, requires_wiredtiger]
 */
(function() {
'use strict';

const newIndexFormatVersion = 12;
const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
const nodes = rst.startSet();
rst.initiate();

let dbName = 'test';
let collName = 't';
const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const secondary = rst.getSecondary();
const coll = primaryDb.getCollection(collName);

assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
assert.writeOK(coll.insert({_id: 0, a: 1}));
assert.commandWorked(primaryDb.adminCommand({setFeatureCompatibilityVersion: '4.0'}));
assert.commandWorked(primaryDb.runCommand({collMod: coll.getName()}));

// Wait for replication of the index creation.
rst.awaitReplication();
const secondaryDb = secondary.getDB(dbName);
const coll_secondary = secondaryDb.getCollection(collName);
const index = coll_secondary.getIndexes();
assert.eq(index[1].unique, true, "Expected a unique index: " + tojson(index[1]));
// Validate that the unique index is not updated on the secondary after an empty collMod
// command.
const indexFormatVersion = coll_secondary.aggregate({$collStats: {storageStats: {}}})
                               .next()
                               .storageStats.indexDetails[index[1].name]
                               .metadata.formatVersion;
assert.eq(indexFormatVersion,
          newIndexFormatVersion,
          "Expected index format version 12 for the unique index: " + tojson(index[1]));

rst.stopSet();
})();
