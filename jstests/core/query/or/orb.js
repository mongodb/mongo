// check neg direction index and negation

let t = db.jstests_orb;
t.drop();

t.save({a: 1});
t.createIndex({a: -1});

assert.eq(1, t.count({$or: [{a: {$gt: 0, $lt: 2}}, {a: {$gt: -1, $lt: 3}}]}));

t.drop();

t.save({a: 1, b: 1});
t.createIndex({a: 1, b: -1});

assert.eq(1, t.count({$or: [{a: {$gt: 0, $lt: 2}}, {a: {$gt: -1, $lt: 3}}]}));
assert.eq(
    1,
    t.count({
        $or: [
            {a: 1, b: {$gt: 0, $lt: 2}},
            {a: 1, b: {$gt: -1, $lt: 3}},
        ],
    }),
);
