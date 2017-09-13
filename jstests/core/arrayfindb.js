// Test $elemMatch object with complex embedded expressions.

var t = db.jstests_arrayfindb;
t.drop();

// Case #1: Ensure correct matching for $elemMatch with an embedded $and (SERVER-13664).
t.save({a: [{b: 1, c: 25}, {a: 3, b: 59}]});
assert.eq(0,
          t.find({a: {$elemMatch: {b: {$gte: 2, $lt: 4}, c: 25}}}).itcount(),
          "Case #1: wrong number of results returned -- unindexed");

t.ensureIndex({"a.b": 1, "a.c": 1});
assert.eq(0,
          t.find({a: {$elemMatch: {b: {$gte: 2, $lt: 4}, c: 25}}}).itcount(),
          "Case #1: wrong number of results returned -- indexed");

// Case #2: Ensure correct matching for $elemMatch with an embedded $or.
t.drop();
t.save({a: [{b: 1}, {c: 1}]});
t.save({a: [{b: 2}, {c: 1}]});
t.save({a: [{b: 1}, {c: 2}]});
assert.eq(2,
          t.find({a: {$elemMatch: {$or: [{b: 2}, {c: 2}]}}}).itcount(),
          "Case #2: wrong number of results returned -- unindexed");

t.ensureIndex({"a.b": 1});
t.ensureIndex({"a.c": 1});
assert.eq(2,
          t.find({a: {$elemMatch: {$or: [{b: 2}, {c: 2}]}}}).itcount(),
          "Case #2: wrong number of results returned -- indexed");
