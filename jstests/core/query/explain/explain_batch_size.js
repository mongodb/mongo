// minimal test to check handling of batch size when explain info is requested
// expected behavior is to return explain.n = total number of documents matching query
// batch size is also tested in another smoke test jstest/query/core/explain/explain1.js but that
// test also covers the use of an indexed collection and includes a couple of test cases using
// limit()
//
// @tags: [
//   requires_fastcount,
//   requires_getmore,
// ]

let t = db.explain_batch_size;
t.drop();

let n = 3;
for (let i = 0; i < n; i++) {
    t.save({x: i});
}

let q = {};

assert.eq(n, t.find(q).count(), "A");
assert.eq(n, t.find(q).itcount(), "B");
assert.eq(n, t.find(q).batchSize(1).explain("executionStats").executionStats.nReturned, "C");
