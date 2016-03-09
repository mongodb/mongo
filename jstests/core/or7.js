t = db.jstests_or7;
t.drop();

t.ensureIndex({a: 1});
t.save({a: 2});

assert.eq.automsg("1", "t.count( {$or:[{a:{$in:[1,3]}},{a:2}]} )");

// SERVER-1201 ...

t.remove({});

t.save({a: "aa"});
t.save({a: "ab"});
t.save({a: "ad"});

assert.eq.automsg("3", "t.count( {$or:[{a:/^ab/},{a:/^a/}]} )");

t.remove({});

t.save({a: "aa"});
t.save({a: "ad"});

assert.eq.automsg("2", "t.count( {$or:[{a:/^ab/},{a:/^a/}]} )");

t.remove({});

t.save({a: "aa"});
t.save({a: "ac"});

assert.eq.automsg("2", "t.count( {$or:[{a:/^ab/},{a:/^a/}]} )");

assert.eq.automsg("2", "t.count( {$or:[{a:/^ab/},{a:/^a/}]} )");

t.save({a: "ab"});
assert.eq.automsg("3", "t.count( {$or:[{a:{$in:[/^ab/],$gte:'abc'}},{a:/^a/}]} )");

t.remove({});
t.save({a: "a"});
t.save({a: "b"});
assert.eq.automsg("2", "t.count( {$or:[{a:{$gt:'a',$lt:'b'}},{a:{$gte:'a',$lte:'b'}}]} )");
