// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
//   requires_getmore,
// ]
const t = db.index_check3;
t.drop();

t.save({a: 1});
t.save({a: 2});
t.save({a: 3});
t.save({a: "z"});

assert.eq(1, t.find({a: {$lt: 2}}).itcount(), "A");
assert.eq(1, t.find({a: {$gt: 2}}).itcount(), "B");

t.createIndex({a: 1});

assert.eq(1, t.find({a: {$lt: 2}}).itcount(), "C");
assert.eq(1, t.find({a: {$gt: 2}}).itcount(), "D");

t.drop();

for (let i = 0; i < 100; i++) {
    let o = {i: i};
    if (i % 2 == 0) o.foo = i;
    t.save(o);
}

t.createIndex({foo: 1});

let explain = t.find({foo: {$lt: 50}}).explain("executionStats");
assert.gt(30, explain.executionStats.totalKeysExamined, "lt");
explain = t.find({foo: {$gt: 50}}).explain("executionStats");
assert.gt(30, explain.executionStats.totalKeysExamined, "gt");

t.drop();
t.save({i: "a"});
for (let i = 0; i < 10; ++i) {
    t.save({});
}

t.createIndex({i: 1});

explain = t.find({i: {$lte: "a"}}).explain("executionStats");
assert.gt(3, explain.executionStats.totalKeysExamined, "lte");

// bug SERVER-99
explain = t.find({i: {$gte: "a"}}).explain("executionStats");
assert.gt(3, explain.executionStats.totalKeysExamined, "gte");
assert.eq(1, t.find({i: {$gte: "a"}}).count(), "gte a");
assert.eq(1, t.find({i: {$gte: "a"}}).itcount(), "gte b");
assert.eq(
    1,
    t
        .find({i: {$gte: "a"}})
        .sort({i: 1})
        .count(),
    "gte c",
);
assert.eq(
    1,
    t
        .find({i: {$gte: "a"}})
        .sort({i: 1})
        .itcount(),
    "gte d",
);

t.save({i: "b"});

explain = t.find({i: {$gte: "a"}}).explain("executionStats");
assert.gt(3, explain.executionStats.totalKeysExamined, "gte");
assert.eq(2, t.find({i: {$gte: "a"}}).count(), "gte a2");
assert.eq(2, t.find({i: {$gte: "a"}}).itcount(), "gte b2");
assert.eq(2, t.find({i: {$gte: "a", $lt: MaxKey}}).itcount(), "gte c2");
assert.eq(
    2,
    t
        .find({i: {$gte: "a", $lt: MaxKey}})
        .sort({i: -1})
        .itcount(),
    "gte d2",
);
assert.eq(
    2,
    t
        .find({i: {$gte: "a", $lt: MaxKey}})
        .sort({i: 1})
        .itcount(),
    "gte e2",
);
