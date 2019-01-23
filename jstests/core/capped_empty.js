// @tags: [
//   requires_fastcount,
//   requires_non_retryable_commands,
//   uses_testing_only_commands,
// ]

t = db.capped_empty;
t.drop();

db.createCollection(t.getName(), {capped: true, size: 100});

t.insert({x: 1});
t.insert({x: 2});
t.insert({x: 3});
t.ensureIndex({x: 1});

assert.eq(3, t.count());

t.runCommand("emptycapped");

assert.eq(0, t.count());

t.insert({x: 1});
t.insert({x: 2});
t.insert({x: 3});

assert.eq(3, t.count());
