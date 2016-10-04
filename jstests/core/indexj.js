// SERVER-726

t = db.jstests_indexj;
t.drop();

function keysExamined(query, hint, sort) {
    if (!hint) {
        hint = {};
    }
    if (!sort) {
        sort = {};
    }
    var explain = t.find(query).sort(sort).hint(hint).explain("executionStats");
    return explain.executionStats.totalKeysExamined;
}

t.ensureIndex({a: 1});
t.save({a: 5});
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "A");

t.drop();
t.ensureIndex({a: 1});
t.save({a: 4});
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "B");

t.save({a: 5});
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "D");

t.save({a: 4});
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "C");

t.save({a: 5});
assert.eq(0, keysExamined({a: {$gt: 4, $lt: 5}}), "D");

t.drop();
t.ensureIndex({a: 1, b: 1});
t.save({a: 1, b: 1});
t.save({a: 1, b: 2});
t.save({a: 2, b: 1});
t.save({a: 2, b: 2});

assert.eq(3, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(3, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1}));

t.save({a: 1, b: 1});
t.save({a: 1, b: 1});
assert.eq(3, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(3, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(3, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1}));

assert.eq(2, keysExamined({a: {$in: [1, 1.9]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}));
assert.eq(2, keysExamined({a: {$in: [1.1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}, {a: -1, b: -1}));

t.save({a: 1, b: 1.5});
assert.eq(4, keysExamined({a: {$in: [1, 2]}, b: {$gt: 1, $lt: 2}}, {a: 1, b: 1}), "F");
