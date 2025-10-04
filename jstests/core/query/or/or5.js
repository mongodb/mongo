// @tags: [
//   requires_getmore,
// ]

let t = db.jstests_or5;
t.drop();

t.createIndex({a: 1});
t.createIndex({b: 1});

t.createIndex({c: 1});

t.save({a: 2});
t.save({b: 3});
t.save({c: 4});
t.save({a: 2, b: 3});
t.save({a: 2, c: 4});
t.save({b: 3, c: 4});
t.save({a: 2, b: 3, c: 4});

assert.eq(7, t.count({$or: [{a: 2}, {b: 3}, {c: 4}]}));
assert.eq(6, t.count({$or: [{a: 6}, {b: 3}, {c: 4}]}));
assert.eq(6, t.count({$or: [{a: 2}, {b: 6}, {c: 4}]}));
assert.eq(6, t.count({$or: [{a: 2}, {b: 3}, {c: 6}]}));

assert.eq(7, t.find({$or: [{a: 2}, {b: 3}, {c: 4}]}).toArray().length);
assert.eq(6, t.find({$or: [{a: 6}, {b: 3}, {c: 4}]}).toArray().length);
assert.eq(6, t.find({$or: [{a: 2}, {b: 6}, {c: 4}]}).toArray().length);
assert.eq(6, t.find({$or: [{a: 2}, {b: 3}, {c: 6}]}).toArray().length);

for (let i = 2; i <= 7; ++i) {
    assert.eq(
        7,
        t
            .find({$or: [{a: 2}, {b: 3}, {c: 4}]})
            .batchSize(i)
            .toArray().length,
    );
    assert.eq(
        6,
        t
            .find({$or: [{a: 6}, {b: 3}, {c: 4}]})
            .batchSize(i)
            .toArray().length,
    );
    assert.eq(
        6,
        t
            .find({$or: [{a: 2}, {b: 6}, {c: 4}]})
            .batchSize(i)
            .toArray().length,
    );
    assert.eq(
        6,
        t
            .find({$or: [{a: 2}, {b: 3}, {c: 6}]})
            .batchSize(i)
            .toArray().length,
    );
}

t.createIndex({z: "2d"});

assert.throws(function () {
    return t.find({$or: [{z: {$near: [50, 50]}}, {a: 2}]}).toArray();
});

function reset() {
    t.drop();

    t.createIndex({a: 1});
    t.createIndex({b: 1});
    t.createIndex({c: 1});

    t.save({a: 2});
    t.save({a: 2});
    t.save({b: 3});
    t.save({b: 3});
    t.save({c: 4});
    t.save({c: 4});
}

reset();

assert.eq(
    6,
    t
        .find({$or: [{a: 2}, {b: 3}, {c: 4}]})
        .batchSize(1)
        .itcount(),
);
assert.eq(
    6,
    t
        .find({$or: [{a: 2}, {b: 3}, {c: 4}]})
        .batchSize(2)
        .itcount(),
);

t.drop();

t.save({a: [1, 2]});
assert.eq(1, t.find({$or: [{a: [1, 2]}]}).itcount());
assert.eq(1, t.find({$or: [{a: {$all: [1, 2]}}]}).itcount());
assert.eq(0, t.find({$or: [{a: {$all: [1, 3]}}]}).itcount());
