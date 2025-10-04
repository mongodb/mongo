// @tags: [
//   requires_non_retryable_writes,
//   requires_getmore,
// ]

// missing collection

let t = db.jstests_or8;
t.drop();

t.find({"$or": [{"PropA": {"$lt": "b"}}, {"PropA": {"$lt": "b", "$gt": "a"}}]}).toArray();

// empty $in

t.save({a: 1});
t.save({a: 3});
t.createIndex({a: 1});
t.find({$or: [{a: {$in: []}}]}).toArray();
assert.eq(t.find({$or: [{a: {$in: []}}, {a: 1}, {a: 3}]}).toArray().length, 2);
assert.eq(t.find({$or: [{a: 1}, {a: {$in: []}}, {a: 3}]}).toArray().length, 2);
assert.eq(t.find({$or: [{a: 1}, {a: 3}, {a: {$in: []}}]}).toArray().length, 2);

// nested negate field

t.drop();
t.save({a: {b: 1, c: 1}});
t.createIndex({"a.b": 1});
t.createIndex({"a.c": 1});
assert.eq(1, t.find({$or: [{"a.b": 1}, {"a.c": 1}]}).itcount());

t.remove({});
t.save({
    a: [
        {b: 1, c: 1},
        {b: 2, c: 1},
    ],
});
assert.eq(1, t.find({$or: [{"a.b": 2}, {"a.c": 1}]}).itcount());
