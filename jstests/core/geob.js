var t = db.geob;
t.drop();

var a = {p: [0, 0]};
var b = {p: [1, 0]};
var c = {p: [3, 4]};
var d = {p: [0, 6]};

t.save(a);
t.save(b);
t.save(c);
t.save(d);
t.ensureIndex({p: "2d"});

var res = t.runCommand("geoNear", {near: [0, 0]});
assert.close(3, res.stats.avgDistance, "A");

assert.close(0, res.results[0].dis, "B1");
assert.eq(a._id, res.results[0].obj._id, "B2");

assert.close(1, res.results[1].dis, "C1");
assert.eq(b._id, res.results[1].obj._id, "C2");

assert.close(5, res.results[2].dis, "D1");
assert.eq(c._id, res.results[2].obj._id, "D2");

assert.close(6, res.results[3].dis, "E1");
assert.eq(d._id, res.results[3].obj._id, "E2");

res = t.runCommand("geoNear", {near: [0, 0], distanceMultiplier: 2});
assert.close(6, res.stats.avgDistance, "F");
assert.close(0, res.results[0].dis, "G");
assert.close(2, res.results[1].dis, "H");
assert.close(10, res.results[2].dis, "I");
assert.close(12, res.results[3].dis, "J");
