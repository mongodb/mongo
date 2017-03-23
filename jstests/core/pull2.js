
t = db.pull2;
t.drop();

t.save({a: [{x: 1}, {x: 1, b: 2}]});
assert.eq(2, t.findOne().a.length, "A");

t.update({}, {$pull: {a: {x: 1}}});
assert.eq(0, t.findOne().a.length, "B");

assert.eq(1, t.find().count(), "C1");

t.update({}, {$push: {a: {x: 1}}});
t.update({}, {$push: {a: {x: 1, b: 2}}});
assert.eq(2, t.findOne().a.length, "C");

t.update({}, {$pullAll: {a: [{x: 1}]}});
assert.eq(1, t.findOne().a.length, "D");

t.update({}, {$push: {a: {x: 2, b: 2}}});
t.update({}, {$push: {a: {x: 3, b: 2}}});
t.update({}, {$push: {a: {x: 4, b: 2}}});
assert.eq(4, t.findOne().a.length, "E");

assert.eq(1, t.find().count(), "C2");

t.update({}, {$pull: {a: {x: {$lt: 3}}}});
assert.eq(2, t.findOne().a.length, "F");
assert.eq([3, 4],
          t.findOne().a.map(function(z) {
              return z.x;
          }),
          "G");
