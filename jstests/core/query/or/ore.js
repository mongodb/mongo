// verify that index direction is considered when deduping based on an earlier
// index

let t = db.jstests_ore;
t.drop();

t.createIndex({a: -1});
t.createIndex({b: 1});

t.save({a: 1, b: 1});
t.save({a: 2, b: 1});

assert.eq(2, t.count({$or: [{a: {$in: [1, 2]}}, {b: 1}]}));
