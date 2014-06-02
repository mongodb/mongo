// Test basic use of collection named "system".  Although the name looks like a "system collection"
// name, it's actually a normal collection that supports all the usual user operations.

var coll = db.getSiblingDB("system_collection_ops").system;
coll.getDB().dropDatabase();

// Test CRUD operations on a collection named "system".
assert.writeOK(coll.insert({}));
assert.writeOK(coll.remove({}));
assert.writeOK(coll.update({}, {$set: {a: 1}}, {upsert: true}));
assert.eq(1, coll.find().itcount());

// Test creating and dropping indexes on a collection named "system".
assert.commandWorked(coll.ensureIndex({a: 1}, {name: "a_1"}));
assert.eq(1, coll.getDB().system.namespaces.count({name: coll + ".$a_1"}));
assert.commandWorked(coll.dropIndex({a: 1}));
assert.eq(0, coll.getDB().system.namespaces.count({name: coll + ".$a_1"})); // SERVER-13975
assert.commandWorked(coll.ensureIndex({a: 1}, {name: "a_1"}));

// Test renaming a collection named "system".
var otherColl = "system2";
assert.commandWorked(coll.renameCollection(otherColl));
assert.commandWorked(coll.getDB().getCollection(otherColl).renameCollection(coll.getName()));
assert.eq(1, coll.count());
assert.eq(2, coll.getIndexes().length);

// Test copying a database with a collection named "system".
var otherDB = "system_collection_ops2";
coll.getDB().getSiblingDB(otherDB).dropDatabase();
assert.commandWorked(coll.getDB().adminCommand({copydb:1, fromdb: coll.getDB().toString(),
                                                todb: otherDB}));
coll.getDB().dropDatabase();
assert.commandWorked(coll.getDB().adminCommand({copydb:1, fromdb: otherDB,
                                                todb: coll.getDB().toString()}));
coll.getDB().getSiblingDB(otherDB).dropDatabase();
assert.eq(1, coll.count());
assert.eq(2, coll.getIndexes().length);
