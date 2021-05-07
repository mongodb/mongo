// @tags: [
//   assumes_balancer_off,
// ]

t = db.regex3;
t.drop();

assert.commandWorked(t.save({name: "eliot"}));
assert.commandWorked(t.save({name: "emily"}));
assert.commandWorked(t.save({name: "bob"}));
assert.commandWorked(t.save({name: "aaron"}));

assert.eq(2, t.find({name: /^e.*/}).itcount(), "no index count");
assert.eq(
    4, t.find({name: /^e.*/}).explain(true).executionStats.totalDocsExamined, "no index explain");
assert.commandWorked(t.createIndex({name: 1}));
assert.eq(2, t.find({name: /^e.*/}).itcount(), "index count");
assert.eq(2,
          t.find({name: /^e.*/}).explain(true).executionStats.totalKeysExamined,
          "index explain");  // SERVER-239

t.drop();

assert.commandWorked(t.save({name: "aa"}));
assert.commandWorked(t.save({name: "ab"}));
assert.commandWorked(t.save({name: "ac"}));
assert.commandWorked(t.save({name: "c"}));

assert.eq(3, t.find({name: /^aa*/}).itcount(), "B ni");
assert.commandWorked(t.createIndex({name: 1}));
assert.eq(3, t.find({name: /^aa*/}).itcount(), "B i 1");
assert.eq(4, t.find({name: /^aa*/}).explain(true).executionStats.totalKeysExamined, "B i 1 e");

assert.eq(2, t.find({name: /^a[ab]/}).itcount(), "B i 2");
assert.eq(2, t.find({name: /^a[bc]/}).itcount(), "B i 3");

t.drop();

assert.commandWorked(t.save({name: ""}));
assert.eq(1, t.find({name: /^a?/}).itcount(), "C 1");
assert.commandWorked(t.createIndex({name: 1}));
assert.eq(1, t.find({name: /^a?/}).itcount(), "C 2");
