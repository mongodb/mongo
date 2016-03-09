
t = db.geo7;
t.drop();

t.insert({_id: 1, y: [1, 1]});
t.insert({_id: 2, y: [1, 1], z: 3});
t.insert({_id: 3, y: [1, 1], z: 4});
t.insert({_id: 4, y: [1, 1], z: 5});

t.ensureIndex({y: "2d", z: 1});

assert.eq(1, t.find({y: [1, 1], z: 3}).itcount(), "A1");

t.dropIndex({y: "2d", z: 1});

t.ensureIndex({y: "2d"});
assert.eq(1, t.find({y: [1, 1], z: 3}).itcount(), "A2");

t.insert({_id: 5, y: 5});
assert.eq(5, t.findOne({y: 5})._id, "B1");
