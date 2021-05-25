/*
 * Test renameCollection functionality across different databases.
 *
 * @tags: [
 *   # Rename between DBs with different shard primary is not supported
 *   assumes_unsharded_collection,
 *   requires_non_retryable_commands,
 *   requires_collstats,
 *   requires_capped,
 *   # On sharded cluster primary shard for database can land in different
 *   # shards and rename across different primary shards is not allowed.
 *   assumes_against_mongod_not_mongos
 * ]
 */

// Set up namespaces a and b.
var db_a = db.getSiblingDB("db_a");
var db_b = db.getSiblingDB("db_b");

var a = db_a.rename7;
var b = db_b.rename7;

// Ensure that the databases are created
db_a.coll.insert({});
db_b.coll.insert({});

a.drop();
b.drop();

// Put some documents and indexes in a.
a.save({a: 1});
a.save({a: 2});
a.save({a: 3});
a.createIndex({a: 1});
a.createIndex({b: 1});

assert.commandWorked(db.adminCommand({renameCollection: "db_a.rename7", to: "db_b.rename7"}));

assert.eq(0, a.countDocuments({}));
assert(db_a.getCollectionNames().indexOf("rename7") < 0);

assert.eq(3, b.countDocuments({}));
assert(db_b.getCollectionNames().indexOf("rename7") >= 0);

a.drop();
b.drop();

// Test that the dropTarget option works when renaming across databases.
a.save({});
b.save({});
assert.commandFailed(db.adminCommand({renameCollection: "db_a.rename7", to: "db_b.rename7"}));
assert.commandWorked(
    db.adminCommand({renameCollection: "db_a.rename7", to: "db_b.rename7", dropTarget: true}));
a.drop();
b.drop();

// Capped collection testing.
db_a.createCollection("rename7_capped", {capped: true, size: 10000});
a = db_a.rename7_capped;
b = db_b.rename7_capped;

a.save({a: 1});
a.save({a: 2});
a.save({a: 3});

previousMaxSize = assert.commandWorked(a.stats()).maxSize;

assert.commandWorked(
    db.adminCommand({renameCollection: "db_a.rename7_capped", to: "db_b.rename7_capped"}));

assert.eq(0, a.countDocuments({}));
assert(db_a.getCollectionNames().indexOf("rename7_capped") < 0);

assert.eq(3, b.countDocuments({}));
assert(db_b.getCollectionNames().indexOf("rename7_capped") >= 0);

stats = assert.commandWorked(db_b.rename7_capped.stats());
printjson(stats);
assert(stats.capped);
assert.eq(previousMaxSize, stats.maxSize);

a.drop();
b.drop();
