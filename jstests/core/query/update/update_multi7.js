// @tags: [requires_multi_updates, requires_non_retryable_writes]

const t = db[jsTestName()];
t.drop();

function s() {
    return t.find().sort({_id: 1}).map(function(z) {
        return z.x;
    });
}

assert.commandWorked(t.save({_id: 1, x: 1}));
assert.commandWorked(t.save({_id: 2, x: 5}));

assert.eq("1,5", s(), "A");

assert.commandWorked(t.update({_id: 1}, {$inc: {x: 1}}));
assert.eq("2,5", s(), "B");

assert.commandWorked(t.update({_id: 1}, {$inc: {x: 1}}));
assert.eq("3,5", s(), "C");

assert.commandWorked(t.update({_id: 2}, {$inc: {x: 1}}));
assert.eq("3,6", s(), "D");

assert.commandWorked(t.update({}, {$inc: {x: 1}}, false, true));
assert.eq("4,7", s(), "E");

assert.commandWorked(t.update({}, {$set: {x: 2}}, false, true));
assert.eq("2,2", s(), "F");

// non-matching in cursor

t.drop();

assert.commandWorked(t.save({_id: 1, x: 1, a: 1, b: 1}));
assert.commandWorked(t.save({_id: 2, x: 5, a: 1, b: 2}));
assert.eq("1,5", s(), "B1");

assert.commandWorked(t.update({a: 1}, {$inc: {x: 1}}, false, true));
assert.eq("2,6", s(), "B2");

assert.commandWorked(t.update({b: 1}, {$inc: {x: 1}}, false, true));
assert.eq("3,6", s(), "B3");

assert.commandWorked(t.update({b: 3}, {$inc: {x: 1}}, false, true));
assert.eq("3,6", s(), "B4");

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1}));

assert.commandWorked(t.update({a: 1}, {$inc: {x: 1}}, false, true));
assert.eq("4,7", s(), "B5");

assert.commandWorked(t.update({b: 1}, {$inc: {x: 1}}, false, true));
assert.eq("5,7", s(), "B6");

assert.commandWorked(t.update({b: 3}, {$inc: {x: 1}}, false, true));
assert.eq("5,7", s(), "B7");

assert.commandWorked(t.update({b: 2}, {$inc: {x: 1}}, false, true));
assert.eq("5,8", s(), "B7");

// multi-key

t.drop();

assert.commandWorked(t.save({_id: 1, x: 1, a: [1, 2]}));
assert.commandWorked(t.save({_id: 2, x: 5, a: [2, 3]}));
assert.eq("1,5", s(), "C1");

assert.commandWorked(t.update({a: 1}, {$inc: {x: 1}}, false, true));
assert.eq("2,5", s(), "C2");

assert.commandWorked(t.update({a: 1}, {$inc: {x: 1}}, false, true));
assert.eq("3,5", s(), "C3");

assert.commandWorked(t.update({a: 3}, {$inc: {x: 1}}, false, true));
assert.eq("3,6", s(), "C4");

assert.commandWorked(t.update({a: 2}, {$inc: {x: 1}}, false, true));
assert.eq("4,7", s(), "C5");

assert.commandWorked(t.update({a: {$gt: 0}}, {$inc: {x: 1}}, false, true));
assert.eq("5,8", s(), "C6");

t.drop();

assert.commandWorked(t.save({_id: 1, x: 1, a: [1, 2]}));
assert.commandWorked(t.save({_id: 2, x: 5, a: [2, 3]}));
assert.commandWorked(t.createIndex({a: 1}));
assert.eq("1,5", s(), "D1");

assert.commandWorked(t.update({a: 1}, {$inc: {x: 1}}, false, true));
assert.eq("2,5", s(), "D2");

assert.commandWorked(t.update({a: 1}, {$inc: {x: 1}}, false, true));
assert.eq("3,5", s(), "D3");

assert.commandWorked(t.update({a: 3}, {$inc: {x: 1}}, false, true));
assert.eq("3,6", s(), "D4");

assert.commandWorked(t.update({a: 2}, {$inc: {x: 1}}, false, true));
assert.eq("4,7", s(), "D5");

assert.commandWorked(t.update({a: {$gt: 0}}, {$inc: {x: 1}}, false, true));
assert.eq("5,8", s(), "D6");

assert.commandWorked(t.update({a: {$lt: 10}}, {$inc: {x: -1}}, false, true));
assert.eq("4,7", s(), "D7");

// ---

assert.commandWorked(t.save({_id: 3}));
assert.eq("4,7,", s(), "E1");
assert.commandWorked(t.update({}, {$inc: {x: 1}}, false, true));
assert.eq("5,8,1", s(), "E2");

let i;
for (i = 4; i < 8; i++)
    assert.commandWorked(t.save({_id: i}));
assert.commandWorked(t.save({_id: i, x: 1}));
assert.eq("5,8,1,,,,,1", s(), "E4");
assert.commandWorked(t.update({}, {$inc: {x: 1}}, false, true));
assert.eq("6,9,2,1,1,1,1,2", s(), "E5");

// --- $inc indexed field

t.drop();

assert.commandWorked(t.save({_id: 1, x: 1}));
assert.commandWorked(t.save({_id: 2, x: 2}));
assert.commandWorked(t.save({_id: 3, x: 3}));

assert.commandWorked(t.createIndex({x: 1}));

assert.eq("1,2,3", s(), "F1");
assert.commandWorked(t.update({x: {$gt: 0}}, {$inc: {x: 5}}, false, true));
assert.eq("6,7,8", s(), "F1");
