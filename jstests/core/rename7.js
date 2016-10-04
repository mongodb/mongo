// ***************************************************************
// rename7.js
// Test renameCollection functionality across different databases.
// ***************************************************************

// Set up namespaces a and b.
var admin = db.getMongo().getDB("admin");
var db_a = db.getMongo().getDB("db_a");
var db_b = db.getMongo().getDB("db_b");

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
a.ensureIndex({a: 1});
a.ensureIndex({b: 1});

assert.commandWorked(admin.runCommand({renameCollection: "db_a.rename7", to: "db_b.rename7"}));

assert.eq(0, a.find().count());
assert(db_a.getCollectionNames().indexOf("rename7") < 0);

assert.eq(3, b.find().count());
assert(db_b.getCollectionNames().indexOf("rename7") >= 0);

a.drop();
b.drop();

// Test that the dropTarget option works when renaming across databases.
a.save({});
b.save({});
assert.commandFailed(admin.runCommand({renameCollection: "db_a.rename7", to: "db_b.rename7"}));
assert.commandWorked(
    admin.runCommand({renameCollection: "db_a.rename7", to: "db_b.rename7", dropTarget: true}));
a.drop();
b.drop();

// Capped collection testing.
db_a.createCollection("rename7_capped", {capped: true, size: 10000});
a = db_a.rename7_capped;
b = db_b.rename7_capped;

a.save({a: 1});
a.save({a: 2});
a.save({a: 3});

previousMaxSize = a.stats().maxSize;

assert.commandWorked(
    admin.runCommand({renameCollection: "db_a.rename7_capped", to: "db_b.rename7_capped"}));

assert.eq(0, a.find().count());
assert(db_a.getCollectionNames().indexOf("rename7_capped") < 0);

assert.eq(3, b.find().count());
assert(db_b.getCollectionNames().indexOf("rename7_capped") >= 0);
printjson(db_b.rename7_capped.stats());
assert(db_b.rename7_capped.stats().capped);
assert.eq(previousMaxSize, b.stats().maxSize);

a.drop();
b.drop();
