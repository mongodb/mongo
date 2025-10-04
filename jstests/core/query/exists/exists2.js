// @tags: [
//   requires_getmore,
// ]

let t = db.exists2;
t.drop();

t.save({a: 1, b: 1});
t.save({a: 1, b: 1, c: 1});

assert.eq(2, t.find().itcount(), "A1");
assert.eq(2, t.find({a: 1, b: 1}).itcount(), "A2");
assert.eq(1, t.find({a: 1, b: 1, c: {"$exists": true}}).itcount(), "A3");
assert.eq(1, t.find({a: 1, b: 1, c: {"$exists": false}}).itcount(), "A4");

t.createIndex({a: 1, b: 1, c: 1});
assert.eq(1, t.find({a: 1, b: 1, c: {"$exists": true}}).itcount(), "B1");
assert.eq(1, t.find({a: 1, b: 1, c: {"$exists": false}}).itcount(), "B2");
