// @tags: [
//   requires_getmore,
// ]

t = db.proj_key1;
t.drop();

as = [];

for (i = 0; i < 10; i++) {
    as.push({a: i});
    t.insert({a: i, b: i});
}

t.createIndex({a: 1});

assert.eq(as, t.find({a: {$gte: 0}}, {a: 1, _id: 0}).sort({a: 1}).toArray());
assert.eq(as, t.find({a: {$gte: 0}}, {a: 1, _id: 0}).sort({a: 1}).batchSize(2).toArray());
