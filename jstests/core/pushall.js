t = db.jstests_pushall;
t.drop();

t.save({_id: 1, a: [1, 2, 3]});
t.update({_id: 1}, {$pushAll: {a: [4]}});
assert.eq([1, 2, 3, 4], t.findOne({_id: 1}).a);
t.update({}, {$pushAll: {a: [4]}});
assert.eq([1, 2, 3, 4, 4], t.findOne({_id: 1}).a);

t.save({_id: 2, a: [1, 2, 3]});
t.update({_id: 2}, {$pushAll: {a: [4, 5]}});
assert.eq([1, 2, 3, 4, 5], t.findOne({_id: 2}).a);
t.update({_id: 2}, {$pushAll: {a: []}});
assert.eq([1, 2, 3, 4, 5], t.findOne({_id: 2}).a);

t.save({_id: 3});
t.update({_id: 3}, {$pushAll: {a: [1, 2]}});
assert.eq([1, 2], t.findOne({_id: 3}).a);
