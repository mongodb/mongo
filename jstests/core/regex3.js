
t = db.regex3;
t.drop();

t.save({name: "eliot"});
t.save({name: "emily"});
t.save({name: "bob"});
t.save({name: "aaron"});

assert.eq(2, t.find({name: /^e.*/}).itcount(), "no index count");
assert.eq(
    4, t.find({name: /^e.*/}).explain(true).executionStats.totalDocsExamined, "no index explain");
t.ensureIndex({name: 1});
assert.eq(2, t.find({name: /^e.*/}).itcount(), "index count");
assert.eq(2,
          t.find({name: /^e.*/}).explain(true).executionStats.totalKeysExamined,
          "index explain");  // SERVER-239

t.drop();

t.save({name: "aa"});
t.save({name: "ab"});
t.save({name: "ac"});
t.save({name: "c"});

assert.eq(3, t.find({name: /^aa*/}).itcount(), "B ni");
t.ensureIndex({name: 1});
assert.eq(3, t.find({name: /^aa*/}).itcount(), "B i 1");
assert.eq(4, t.find({name: /^aa*/}).explain(true).executionStats.totalKeysExamined, "B i 1 e");

assert.eq(2, t.find({name: /^a[ab]/}).itcount(), "B i 2");
assert.eq(2, t.find({name: /^a[bc]/}).itcount(), "B i 3");

t.drop();

t.save({name: ""});
assert.eq(1, t.find({name: /^a?/}).itcount(), "C 1");
t.ensureIndex({name: 1});
assert.eq(1, t.find({name: /^a?/}).itcount(), "C 2");
