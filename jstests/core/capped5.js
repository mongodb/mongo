
tn = "capped5";

t = db[tn];
t.drop();

db.createCollection(tn, {capped: true, size: 1024 * 1024 * 1});
t.insert({_id: 5, x: 11, z: 52});
assert.eq(1, t.getIndexKeys().length, "A0");  // now we assume _id index even on capped coll
assert.eq(52, t.findOne({x: 11}).z, "A1");

t.ensureIndex({_id: 1});
t.ensureIndex({x: 1});

assert.eq(52, t.findOne({x: 11}).z, "B1");
assert.eq(52, t.findOne({_id: 5}).z, "B2");

t.drop();
db.createCollection(tn, {capped: true, size: 1024 * 1024 * 1});
t.insert({_id: 5, x: 11});
t.insert({_id: 5, x: 12});
assert.eq(1, t.getIndexes().length);      // now we assume _id index
assert.eq(1, t.find().toArray().length);  //_id index unique, so second insert fails

t.drop();
db.createCollection(tn, {capped: true, size: 1024 * 1024 * 1});
t.insert({_id: 5, x: 11});
t.insert({_id: 6, x: 12});
t.ensureIndex({x: 1}, {unique: true});
assert.eq(2, t.getIndexes().length);  // now we assume _id index
assert.eq(2, t.find().hint({x: 1}).toArray().length);

// SERVER-525 (closed) unique indexes in capped collection
t.drop();
db.createCollection(tn, {capped: true, size: 1024 * 1024 * 1});
t.ensureIndex({_id: 1});  // note we assume will be automatically unique because it is _id
t.insert({_id: 5, x: 11});
t.insert({_id: 5, x: 12});
assert.eq(1, t.find().toArray().length);
