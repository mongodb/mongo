// @tags: [requires_non_retryable_writes]

// Check normal count matching and deduping.

let t = db.jstests_count7;
t.drop();

t.createIndex({a: 1});
t.save({a: "algebra"});
t.save({a: "apple"});
t.save({a: "azores"});
t.save({a: "bumper"});
t.save({a: "supper"});
t.save({a: "termite"});
t.save({a: "zeppelin"});
t.save({a: "ziggurat"});
t.save({a: "zope"});

assert.eq(5, t.count({a: /p/}));

t.remove({});

t.save({a: [1, 2, 3]});
t.save({a: [1, 2, 3]});
t.save({a: [1]});

assert.eq(2, t.count({a: {$gt: 1}}));
