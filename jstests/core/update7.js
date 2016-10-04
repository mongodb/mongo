
t = db.update7;
t.drop();

function s() {
    return t.find().sort({_id: 1}).map(function(z) {
        return z.x;
    });
}

t.save({_id: 1, x: 1});
t.save({_id: 2, x: 5});

assert.eq("1,5", s(), "A");

t.update({}, {$inc: {x: 1}});
assert.eq("2,5", s(), "B");

t.update({_id: 1}, {$inc: {x: 1}});
assert.eq("3,5", s(), "C");

t.update({_id: 2}, {$inc: {x: 1}});
assert.eq("3,6", s(), "D");

t.update({}, {$inc: {x: 1}}, false, true);
assert.eq("4,7", s(), "E");

t.update({}, {$set: {x: 2}}, false, true);
assert.eq("2,2", s(), "F");

// non-matching in cursor

t.drop();

t.save({_id: 1, x: 1, a: 1, b: 1});
t.save({_id: 2, x: 5, a: 1, b: 2});
assert.eq("1,5", s(), "B1");

t.update({a: 1}, {$inc: {x: 1}}, false, true);
assert.eq("2,6", s(), "B2");

t.update({b: 1}, {$inc: {x: 1}}, false, true);
assert.eq("3,6", s(), "B3");

t.update({b: 3}, {$inc: {x: 1}}, false, true);
assert.eq("3,6", s(), "B4");

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

t.update({a: 1}, {$inc: {x: 1}}, false, true);
assert.eq("4,7", s(), "B5");

t.update({b: 1}, {$inc: {x: 1}}, false, true);
assert.eq("5,7", s(), "B6");

t.update({b: 3}, {$inc: {x: 1}}, false, true);
assert.eq("5,7", s(), "B7");

t.update({b: 2}, {$inc: {x: 1}}, false, true);
assert.eq("5,8", s(), "B7");

// multi-key

t.drop();

t.save({_id: 1, x: 1, a: [1, 2]});
t.save({_id: 2, x: 5, a: [2, 3]});
assert.eq("1,5", s(), "C1");

t.update({a: 1}, {$inc: {x: 1}}, false, true);
assert.eq("2,5", s(), "C2");

t.update({a: 1}, {$inc: {x: 1}}, false, true);
assert.eq("3,5", s(), "C3");

t.update({a: 3}, {$inc: {x: 1}}, false, true);
assert.eq("3,6", s(), "C4");

t.update({a: 2}, {$inc: {x: 1}}, false, true);
assert.eq("4,7", s(), "C5");

t.update({a: {$gt: 0}}, {$inc: {x: 1}}, false, true);
assert.eq("5,8", s(), "C6");

t.drop();

t.save({_id: 1, x: 1, a: [1, 2]});
t.save({_id: 2, x: 5, a: [2, 3]});
t.ensureIndex({a: 1});
assert.eq("1,5", s(), "D1");

t.update({a: 1}, {$inc: {x: 1}}, false, true);
assert.eq("2,5", s(), "D2");

t.update({a: 1}, {$inc: {x: 1}}, false, true);
assert.eq("3,5", s(), "D3");

t.update({a: 3}, {$inc: {x: 1}}, false, true);
assert.eq("3,6", s(), "D4");

t.update({a: 2}, {$inc: {x: 1}}, false, true);
assert.eq("4,7", s(), "D5");

t.update({a: {$gt: 0}}, {$inc: {x: 1}}, false, true);
assert.eq("5,8", s(), "D6");

t.update({a: {$lt: 10}}, {$inc: {x: -1}}, false, true);
assert.eq("4,7", s(), "D7");

// ---

t.save({_id: 3});
assert.eq("4,7,", s(), "E1");
t.update({}, {$inc: {x: 1}}, false, true);
assert.eq("5,8,1", s(), "E2");

for (i = 4; i < 8; i++)
    t.save({_id: i});
t.save({_id: i, x: 1});
assert.eq("5,8,1,,,,,1", s(), "E4");
t.update({}, {$inc: {x: 1}}, false, true);
assert.eq("6,9,2,1,1,1,1,2", s(), "E5");

// --- $inc indexed field

t.drop();

t.save({x: 1});
t.save({x: 2});
t.save({x: 3});

t.ensureIndex({x: 1});

assert.eq("1,2,3", s(), "F1");
t.update({x: {$gt: 0}}, {$inc: {x: 5}}, false, true);
assert.eq("6,7,8", s(), "F1");
