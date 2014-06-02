// Test that, during startup, mongod correctly cleans up entries in system.namespaces for orphaned
// indexes on collections named "system" (SERVER-13975).

var collName = "test.system";

// 1a.
// In 2.4, create collection named "system" and drop it.  This exhibits the bug: entries in
// system.namespaces for indexes on this collection are not cleaned up.
var mongod = MongoRunner.runMongod({binVersion: "2.4"});
var coll = mongod.getCollection(collName);
coll.insert({});
assert.gleSuccess(coll.getDB());
assert.isnull(coll.ensureIndex({a: 1}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$_id_"}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$a_1"}));
assert.commandWorked(coll.runCommand("drop"));
assert.commandFailed(coll.runCommand("collStats"));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$_id_"}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$a_1"}));
MongoRunner.stopMongod(mongod);

// 1b.
// Upgrade from 2.4 to "latest".  After mongod starts, the database should still exist but the
// orphaned index entries should be cleaned up.  Furthermore, recreating the collection and building
// indexes on it should work as expected.
mongod = MongoRunner.runMongod({binVersion: "latest", restart: true});
coll = mongod.getCollection(collName);
var pred = function(i) { return i.name === coll.getDB().toString() && i.empty === false; }
assert.eq(1, coll.getDB().adminCommand({listDatabases: 1}).databases.filter(pred).length);
assert.eq(0, coll.getDB().system.namespaces.count({name: collName + ".$_id_"}));
assert.eq(0, coll.getDB().system.namespaces.count({name: collName + ".$a_1"}));
coll.insert({});
assert.gleSuccess(coll.getDB());
var validateRes = coll.validate(true);
assert.eq(1, validateRes.ok);
assert.eq(1, validateRes.nIndexes);
assert.eq(1, validateRes.keysPerIndex[collName + ".$_id_"]);
assert.isnull(coll.ensureIndex({a: 1}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$_id_"}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$a_1"}));
MongoRunner.stopMongod(mongod);

// 2a.
// In 2.4, create collection named "system" and drop an index.  This exhibits the same bug: the
// system.namespaces entry for this index will remain.
mongod = MongoRunner.runMongod({binVersion: "2.4"});
coll = mongod.getCollection(collName);
coll.insert({});
assert.gleSuccess(coll.getDB());
assert.isnull(coll.ensureIndex({a: 1}));
assert.commandWorked(coll.dropIndex({a: 1}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$_id_"}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$a_1"}));
MongoRunner.stopMongod(mongod);

// 2b.
// Upgrade from 2.4 to "latest".  After mongod starts, the orphaned index entry will be cleaned up.
// Recreating it should work as expected.
mongod = MongoRunner.runMongod({binVersion: "latest", restart: true});
coll = mongod.getCollection(collName);
assert.commandWorked(coll.runCommand("collStats"));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$_id_"}));
assert.eq(0, coll.getDB().system.namespaces.count({name: collName + ".$a_1"}));
var validateRes = coll.validate(true);
assert.eq(1, validateRes.ok);
assert.eq(1, validateRes.nIndexes);
assert.eq(1, validateRes.keysPerIndex[collName + ".$_id_"]);
assert.isnull(coll.ensureIndex({a: 1}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$_id_"}));
assert.eq(1, coll.getDB().system.namespaces.count({name: collName + ".$a_1"}));
MongoRunner.stopMongod(mongod);
