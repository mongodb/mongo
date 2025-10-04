// @tags: [
//   assumes_balancer_off,
//   requires_non_retryable_writes,
//   requires_fcv_80,
// ]

// Basic test which checks the number of documents returned, keys examined, and documents
// examined as reported by explain.

const colName = jsTestName();
let t = db.getCollection(colName);
t.drop();

t.createIndex({a: 1, b: 1});
t.createIndex({b: 1, a: 1});

t.save({a: 0, b: 1});
t.save({a: 1, b: 0});

let explain = t.find({a: {$gte: 0}, b: {$gte: 0}}).explain(true);

assert.eq(2, explain.executionStats.nReturned);
assert.eq(2, explain.executionStats.totalKeysExamined);
assert.eq(2, explain.executionStats.totalDocsExamined);

// A limit of 2.
explain = t
    .find({a: {$gte: 0}, b: {$gte: 0}})
    .limit(-2)
    .explain(true);
assert.eq(2, explain.executionStats.nReturned);

// A $or query.
explain = t
    .find({
        $or: [
            {a: {$gte: 0}, b: {$gte: 1}},
            {a: {$gte: 1}, b: {$gte: 0}},
        ],
    })
    .explain(true);
assert.eq(2, explain.executionStats.nReturned);

// A non $or case where totalKeysExamined != number of results
t.remove({});

t.save({a: "0", b: "1"});
t.save({a: "1", b: "0"});
explain = t.find({a: /0/, b: /1/}).explain(true);
assert.eq(1, explain.executionStats.nReturned);
assert.eq(2, explain.executionStats.totalKeysExamined);
