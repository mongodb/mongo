// Check that it's possible to compare a Date to a Timestamp, but they are never equal - SERVER-3304

t = db.jstests_date2;
t.drop();

t.ensureIndex({a: 1});

var obj = {a: new Timestamp(0, 1)};  // in old versions this was == to new Date(1)
t.save(obj);
assert.eq(0, t.find({a: {$gt: new Date(1)}}).itcount());
assert.eq(1, t.find(obj).itcount());
