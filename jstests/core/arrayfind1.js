
t = db.arrayfind1;
t.drop();

t.save({a: [{x: 1}]});
t.save({a: [{x: 1, y: 2, z: 1}]});
t.save({a: [{x: 1, y: 1, z: 3}]});

function test(exptected, q, name) {
    assert.eq(exptected, t.find(q).itcount(), name + " " + tojson(q) + " itcount");
    assert.eq(exptected, t.find(q).count(), name + " " + tojson(q) + " count");
}

test(3, {}, "A1");
test(1, {"a.y": 2}, "A2");
test(1, {"a": {x: 1}}, "A3");
test(3, {"a": {$elemMatch: {x: 1}}}, "A4");  // SERVER-377

t.save({a: [{x: 2}]});
t.save({a: [{x: 3}]});
t.save({a: [{x: 4}]});

assert.eq(1, t.find({a: {$elemMatch: {x: 2}}}).count(), "B1");
assert.eq(2, t.find({a: {$elemMatch: {x: {$gt: 2}}}}).count(), "B2");

t.ensureIndex({"a.x": 1});
assert.eq(1, t.find({a: {$elemMatch: {x: 2}}}).count(), "D1");
assert.eq(3, t.find({"a.x": 1}).count(), "D2.1");
assert.eq(3, t.find({"a.x": {$gt: 1}}).count(), "D2.2");
assert.eq(2, t.find({a: {$elemMatch: {x: {$gt: 2}}}}).count(), "D3");

assert.eq(2, t.find({a: {$ne: 2, $elemMatch: {x: {$gt: 2}}}}).count(), "E1");
