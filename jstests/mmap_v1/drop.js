var coll = db.jstests_drop;

coll.drop();

res = coll.runCommand("drop");
assert(!res.ok, tojson(res));

assert.eq(0, db.system.indexes.find({ns: coll + ""}).count(), "A");
coll.save({});
assert.eq(1, db.system.indexes.find({ns: coll + ""}).count(), "B");
coll.ensureIndex({a: 1});
assert.eq(2, db.system.indexes.find({ns: coll + ""}).count(), "C");
assert.commandWorked(db.runCommand({drop: coll.getName()}));
assert.eq(0, db.system.indexes.find({ns: coll + ""}).count(), "D");

coll.ensureIndex({a: 1});
assert.eq(2, db.system.indexes.find({ns: coll + ""}).count(), "E");
assert.commandWorked(db.runCommand({deleteIndexes: coll.getName(), index: "*"}),
                     "delete indexes A");
assert.eq(1, db.system.indexes.find({ns: coll + ""}).count(), "G");

// make sure we can still use it
coll.save({});
assert.eq(1, coll.find().hint("_id_").toArray().length, "H");
