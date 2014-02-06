//
// Example test for the gle core suite (used in passthroughs)
//

var coll = db.getCollection("gle_example");
coll.drop();

coll.insert({ hello : "world" });
assert.eq( null, coll.getDB().getLastError() );