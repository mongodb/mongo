//
// Example test for the gle core suite (used in passthroughs)
//

var coll = db.getCollection("gle_example");
coll.drop();

coll.insert({hello: "world"});
assert.eq(null, coll.getDB().getLastError());

// Error on insert.
coll.drop();
coll.insert({_id: 1});
coll.insert({_id: 1});
var gle = db.getLastErrorObj();
assert.neq(null, gle.err);

// New requests should clear gle.
coll.findOne();
gle = db.getLastErrorObj();
assert.eq(null, gle.err);

// Error on upsert.
coll.drop();
coll.insert({_id: 1});
coll.update({y: 1}, {_id: 1}, true);
gle = db.getLastErrorObj();
assert.neq(null, gle.err);
