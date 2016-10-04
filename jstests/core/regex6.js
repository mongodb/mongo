// contributed by Andrew Kempe
t = db.regex6;
t.drop();

t.save({name: "eliot"});
t.save({name: "emily"});
t.save({name: "bob"});
t.save({name: "aaron"});
t.save({name: "[with]some?symbols"});

t.ensureIndex({name: 1});

assert.eq(0, t.find({name: /^\//}).count(), "index count");
assert.eq(
    1, t.find({name: /^\//}).explain(true).executionStats.totalKeysExamined, "index explain 1");
assert.eq(
    0, t.find({name: /^é/}).explain(true).executionStats.totalKeysExamined, "index explain 2");
assert.eq(
    0, t.find({name: /^\é/}).explain(true).executionStats.totalKeysExamined, "index explain 3");
assert.eq(
    1, t.find({name: /^\./}).explain(true).executionStats.totalKeysExamined, "index explain 4");
assert.eq(
    5, t.find({name: /^./}).explain(true).executionStats.totalKeysExamined, "index explain 5");

// SERVER-2862
assert.eq(0, t.find({name: /^\Qblah\E/}).count(), "index explain 6");
assert.eq(1,
          t.find({name: /^\Qblah\E/}).explain(true).executionStats.totalKeysExamined,
          "index explain 6");
assert.eq(
    1, t.find({name: /^blah/}).explain(true).executionStats.totalKeysExamined, "index explain 6");
assert.eq(1, t.find({name: /^\Q[\Ewi\Qth]some?s\Eym/}).count(), "index count 2");
assert.eq(2,
          t.find({name: /^\Q[\Ewi\Qth]some?s\Eym/}).explain(true).executionStats.totalKeysExamined,
          "index explain 6");
assert.eq(2,
          t.find({name: /^bob/}).explain(true).executionStats.totalKeysExamined,
          "index explain 6");  // proof executionStats.totalKeysExamined == count+1

assert.eq(
    1,
    t.find({name: {$regex: "^e", $gte: "emily"}}).explain(true).executionStats.totalKeysExamined,
    "ie7");
assert.eq(
    1,
    t.find({name: {$gt: "a", $regex: "^emily"}}).explain(true).executionStats.totalKeysExamined,
    "ie7");
