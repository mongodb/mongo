
t = db.update_invalid1;
t.drop();

t.update({_id: 5}, {$set: {$inc: {x: 5}}}, true);
assert.eq(0, t.count(), "A1");
