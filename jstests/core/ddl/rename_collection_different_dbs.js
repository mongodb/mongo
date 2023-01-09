/*
 * Test renameCollection functionality across different databases.
 *
 * @tags: [
 *   # On sharded cluster primary shard for database can land in different
 *   # shards and rename across different primary shards is not allowed.
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_index_creation,
 *   # Rename across DBs with different shard primary is not supported
 *   assumes_unsharded_collection,
 *   requires_capped,
 *   requires_collstats,
 *   requires_non_retryable_commands,
 *   uses_rename,
 * ]
 */

(function() {
'use strict';

// Set up namespaces a and b.
const db_a = db.getSiblingDB("db_a");
const db_b = db.getSiblingDB("db_b");

let a = db_a.rename_different_db;
let b = db_b.rename_different_db;

a.drop();
b.drop();

// Put some documents and indexes in a.
a.insertMany([{a: 1}, {a: 2}, {a: 3}]);
assert.commandWorked(a.createIndexes([{a: 1}, {b: 1}]));

assert.commandWorked(db.adminCommand(
    {renameCollection: "db_a.rename_different_db", to: "db_b.rename_different_db"}));

assert.eq(0, a.countDocuments({}));
assert(db_a.getCollectionNames().indexOf(a.getName()) < 0);

assert.eq(3, b.countDocuments({}));
assert(db_b.getCollectionNames().indexOf(a.getName()) >= 0);

a.drop();
b.drop();

// Test that the dropTarget option works when renaming across databases.
a.save({});
b.save({});
assert.commandFailed(db.adminCommand(
    {renameCollection: "db_a.rename_different_db", to: "db_b.rename_different_db"}));
assert.commandWorked(db.adminCommand({
    renameCollection: "db_a.rename_different_db",
    to: "db_b.rename_different_db",
    dropTarget: true
}));
a.drop();
b.drop();

// Capped collection testing.
db_a.createCollection("rename_capped", {capped: true, size: 10000});
a = db_a.rename_capped;
b = db_b.rename_capped;

a.insertMany([{a: 1}, {a: 2}, {a: 3}]);
let previousMaxSize = assert.commandWorked(a.stats()).maxSize;

assert.commandWorked(
    db.adminCommand({renameCollection: "db_a.rename_capped", to: "db_b.rename_capped"}));

assert.eq(0, a.countDocuments({}));
assert(db_a.getCollectionNames().indexOf(a.getName()) < 0);

assert.eq(3, b.countDocuments({}));
assert(db_b.getCollectionNames().indexOf(a.getName()) >= 0);

let stats = assert.commandWorked(db_b.rename_capped.stats());
printjson(stats);
assert(stats.capped);
assert.eq(previousMaxSize, stats.maxSize);

a.drop();
b.drop();
})();
